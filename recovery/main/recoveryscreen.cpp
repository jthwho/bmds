/**
 * @file      recoveryscreen.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Recovery's screen (devplan/update.md §12). Text goes through a strip of internal DMA RAM, one
 * row of cells high, and is copied to the surface; the bands and the bar are fills. One task
 * draws what the terminal's changes, the update's progress and the start ask for.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/display.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/input.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/screenconsole.h>
#include <bmds/semaphore.h>
#include <bmds/system.h>
#include <bmds/terminalkeys.h>
#include <bmds/terminalview.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/videomodes.h>
#include "recoveryscreen.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Width = 1280;
        constexpr uint16_t Height = 720;
        constexpr uint16_t CellWidth = 8;
        constexpr uint16_t CellHeight = 16;
        constexpr size_t   StripStride = size_t(Width) * 3;
        constexpr uint16_t Margin = 16; // the text's left edge in the band and the panel
        constexpr uint16_t TextWidth = Width - 2 * Margin;

        // The band, the panel, a rule, then the terminal.
        constexpr uint16_t BandHeight = 32;
        constexpr uint16_t PanelTop = BandHeight;
        constexpr uint16_t PanelHeight = 104;
        constexpr uint16_t RuleTop = PanelTop + PanelHeight;
        constexpr uint16_t RuleHeight = 2;
        constexpr uint16_t TerminalX = 8;
        constexpr uint16_t TerminalY = RuleTop + RuleHeight + 6;
        constexpr uint16_t Columns = (Width - 2 * TerminalX) / CellWidth;
        constexpr uint16_t Rows = (Height - TerminalY - 4) / CellHeight;
        constexpr uint16_t BarWidth = 1000;
        constexpr uint16_t BarHeight = 12;

        constexpr uint32_t BandBackground = 0x6a1010;
        constexpr uint32_t BandText = 0xffc040;
        constexpr uint32_t PanelBackground = 0x1c1c1c;
        constexpr uint32_t PanelText = 0xe8e8e8;
        constexpr uint32_t PanelDim = 0x9c9c9c;
        constexpr uint32_t BarBackground = 0x3c3c3c;
        constexpr uint32_t BarFill = 0xffb020;
        constexpr uint32_t GoodText = 0x70e070;
        constexpr uint32_t BadText = 0xff7070;
        constexpr uint32_t TerminalBackground = 0x000000;

        constexpr unsigned DrawTimeoutMs = 1000;

        // Panel redraws the panel whole (a phase changed); Progress only the stage's line and the bar,
        // which replace their pixels in one go: with one buffer, a fill first would flicker.
        enum Dirty : uint32_t {
                Band = 1u << 0,
                Panel = 1u << 1,
                Progress = 1u << 2,
                Terminal = 1u << 3
        };

        // The panel's lines.
        constexpr uint16_t Line0 = PanelTop + 12;
        constexpr uint16_t Line1 = Line0 + 22;
        constexpr uint16_t Line2 = Line1 + 24;

        enum class Phase : uint8_t {
                Idle,
                Running,
                Applied,
                FailedRestarting,
                FailedStaying
        };

        struct State {
                        State() : wake(1, 0), console("screen", MemSpace::Psram) {}

                        Surface              *surface = nullptr;
                        Semaphore             wake;
                        ScreenConsole         console;
                        Thread                drawer;
                        std::atomic<uint32_t> dirty{Band | Panel | Terminal};
                        std::atomic<bool>     applicationCursor{false};
                        // The update, as the observer hears it.
                        std::atomic<Phase>         phase{Phase::Idle};
                        std::atomic<Update::Stage> stage{Update::Stage::Checking};
                        std::atomic<uint32_t>      permille{0};
                        char                       version[33] = {};
                        const char                *failure = nullptr; // written before `phase`
                        // The drawing task's.
                        uint8_t         *strip = nullptr; // a row of cells, internal DMA RAM
                        TextCell        *cells = nullptr; // the terminal's, copied under its lock
                        uint32_t         rowHash[Rows] = {};
                        Input::Sink      sink;
                        Update::Observer observer;
        };

        State *state = nullptr;

        void poke(uint32_t bits) {
                state->dirty.fetch_or(bits, std::memory_order_acq_rel);
                state->wake.release();
                return;
        }

        // The strip's pixels [from, to) of every row in @p rgb.
        void fillStrip(uint8_t *strip, uint16_t from, uint16_t to, uint32_t rgb) {
                for (uint16_t y = 0; y < CellHeight; ++y) {
                        uint8_t *p = strip + size_t(y) * StripStride + size_t(from) * 3;
                        for (uint16_t x = from; x < to; ++x) {
                                *p++ = uint8_t(rgb);
                                *p++ = uint8_t(rgb >> 8);
                                *p++ = uint8_t(rgb >> 16);
                        }
                }
                return;
        }

        // A line of text at (Margin, @p y), across the text's width, @p fg on @p bg.
        void drawLine(GlyphRenderer &renderer, uint16_t y, const char *text, uint32_t fg, uint32_t bg) {
                size_t         used = 0;
                const uint16_t w = renderer.drawText(StringView(text), Fonts::unscii16, fg, bg, state->strip,
                                                     StripStride, TextWidth, used);
                fillStrip(state->strip, w, TextWidth, bg);
                (void)state->surface->blit(Rect{Margin, y, TextWidth, CellHeight}, state->strip, StripStride,
                                           DrawTimeoutMs);
                return;
        }

        void drawBand(GlyphRenderer &renderer) {
                (void)state->surface->fill(Rect{0, 0, Width, BandHeight}, BandBackground, DrawTimeoutMs);
                const SystemInfo info = System::info();
                char             line[Columns + 1];
                const int        left = std::snprintf(line, sizeof(line), "Recovery Mode");
                char             right[64];
                const int r = std::snprintf(right, sizeof(right), "BMDS %s  %s", info.buildVersion, info.boardName);
                const int columns = TextWidth / CellWidth;
                if (left > 0 && r > 0 && left + r < columns) {
                        std::memset(line + left, ' ', size_t(columns - left - r));
                        std::memcpy(line + columns - r, right, size_t(r) + 1);
                }
                drawLine(renderer, (BandHeight - CellHeight) / 2, line, BandText, BandBackground);
                (void)state->surface->fill(Rect{0, RuleTop, Width, RuleHeight}, BandBackground, DrawTimeoutMs);
                return;
        }

        void drawBar(GlyphRenderer &renderer, uint16_t y, uint32_t permille) {
                const uint16_t filled = uint16_t(uint32_t(BarWidth) * (permille > 1000 ? 1000 : permille) / 1000);
                const uint16_t barY = uint16_t(y + (CellHeight - BarHeight) / 2);
                if (filled > 0)
                        (void)state->surface->fill(Rect{Margin, barY, filled, BarHeight}, BarFill, DrawTimeoutMs);
                if (filled < BarWidth) {
                        (void)state->surface->fill(
                                Rect{uint16_t(Margin + filled), barY, uint16_t(BarWidth - filled), BarHeight},
                                BarBackground, DrawTimeoutMs);
                }
                char percent[16];
                std::snprintf(percent, sizeof(percent), "%3u%%", static_cast<unsigned>(permille / 10));
                size_t         used = 0;
                const uint16_t textX = Margin + BarWidth + 16;
                const uint16_t w = renderer.drawText(StringView(percent), Fonts::unscii16, PanelText, PanelBackground,
                                                     state->strip, StripStride, 64, used);
                fillStrip(state->strip, w, 64, PanelBackground);
                (void)state->surface->blit(Rect{textX, y, 64, CellHeight}, state->strip, StripStride, DrawTimeoutMs);
                return;
        }

        void drawProgress(GlyphRenderer &renderer) {
                char line[Columns + 1];
                std::snprintf(line, sizeof(line), "%s. Don't turn the power off.",
                              Update::stageText(state->stage.load(std::memory_order_relaxed)));
                drawLine(renderer, Line1, line, PanelDim, PanelBackground);
                drawBar(renderer, Line2, state->permille.load(std::memory_order_relaxed));
                return;
        }

        void drawPanel(GlyphRenderer &renderer) {
                (void)state->surface->fill(Rect{0, PanelTop, Width, PanelHeight}, PanelBackground, DrawTimeoutMs);
                const uint16_t y0 = Line0, y1 = Line1, y2 = Line2;
                char           line[Columns + 1];
                const Phase    phase = state->phase.load(std::memory_order_acquire);
                switch (phase) {
                        case Phase::Idle:
                                drawLine(renderer, y0, "No update is waiting.", PanelText, PanelBackground);
                                drawLine(renderer, y1,
                                         "Install one from the card with the keyboard below: update check FILE, "
                                         "then update apply FILE. update status shows the last one.",
                                         PanelDim, PanelBackground);
                                drawLine(renderer, y2, "reboot starts the system again, if it is whole.", PanelDim,
                                         PanelBackground);
                                break;
                        case Phase::Running: {
                                std::snprintf(line, sizeof(line), "Installing BMDS %s", state->version);
                                drawLine(renderer, y0, line, PanelText, PanelBackground);
                                drawProgress(renderer);
                                break;
                        }
                        case Phase::Applied:
                                std::snprintf(line, sizeof(line), "BMDS %s is installed.", state->version);
                                drawLine(renderer, y0, line, GoodText, PanelBackground);
                                drawLine(renderer, y1, "Restarting...", PanelDim, PanelBackground);
                                drawBar(renderer, y2, 1000);
                                break;
                        case Phase::FailedRestarting:
                                std::snprintf(line, sizeof(line), "The update wasn't installed: %s.", state->failure);
                                drawLine(renderer, y0, line, BadText, PanelBackground);
                                drawLine(renderer, y1, "The system is as it was. Restarting it...", PanelDim,
                                         PanelBackground);
                                break;
                        case Phase::FailedStaying:
                                if (std::strcmp(state->failure, Update::SystemDidntStart) == 0) {
                                        // No update ran: recovery was started because the system didn't.
                                        drawLine(renderer, y0, "The system didn't start.", BadText, PanelBackground);
                                        drawLine(renderer, y1,
                                                 "Install an update from the card with the keyboard below: update "
                                                 "apply FILE (update check FILE first).",
                                                 PanelDim, PanelBackground);
                                        drawLine(renderer, y2, "reboot tries the system again.", PanelDim,
                                                 PanelBackground);
                                        break;
                                }
                                std::snprintf(line, sizeof(line), "The update wasn't installed: %s.", state->failure);
                                drawLine(renderer, y0, line, BadText, PanelBackground);
                                drawLine(renderer, y1,
                                         "The system can't start until an update is installed. Put a card with one in "
                                         "and type update apply FILE,",
                                         PanelDim, PanelBackground);
                                drawLine(renderer, y2, "or restart to try the same one again.", PanelDim,
                                         PanelBackground);
                                break;
                }
                return;
        }

        uint32_t hashCells(const TextCell *cells, size_t n) {
                uint32_t h = 2166136261u;
                for (size_t i = 0; i < n; ++i) {
                        const uint32_t words[3] = {cells[i].codepoint, cells[i].fg,
                                                   cells[i].bg ^ (uint32_t(cells[i].flags) << 24)};
                        for (uint32_t w : words) h = (h ^ w) * 16777619u;
                }
                return h;
        }

        void drawTerminal(GlyphRenderer &renderer) {
                // Changes from here on ask again: none is lost between the copy and this.
                state->console.drawn();
                const TerminalColors colors;
                {
                        ScreenConsole::Locked lock(state->console);
                        TerminalScreen       &screen = lock.screen();
                        state->applicationCursor.store(screen.isApplicationCursorKeys(), std::memory_order_relaxed);
                        const int cursorRow = screen.isCursorVisible() ? screen.cursorRow() : -1;
                        for (uint16_t r = 0; r < Rows && r < screen.rows(); ++r) {
                                TextCell    *row = state->cells + size_t(r) * Columns;
                                const size_t n = TerminalView::row(
                                        screen, r, colors, r == cursorRow ? screen.cursorColumn() : -1, row, Columns);
                                for (size_t i = n; i < Columns; ++i)
                                        row[i] = TextCell{' ', colors.text, colors.background, 0};
                        }
                }
                for (uint16_t r = 0; r < Rows; ++r) {
                        const TextCell *row = state->cells + size_t(r) * Columns;
                        const uint32_t  h = hashCells(row, Columns);
                        if (h == state->rowHash[r]) continue;
                        state->rowHash[r] = h;
                        size_t used = 0;
                        (void)renderer.drawCells(Span<const TextCell>(row, Columns), Fonts::unscii16, state->strip,
                                                 StripStride, Columns * CellWidth, used);
                        (void)state->surface->blit(
                                Rect{TerminalX, uint16_t(TerminalY + r * CellHeight), Columns * CellWidth, CellHeight},
                                state->strip, StripStride, DrawTimeoutMs);
                }
                return;
        }

        void drawMain(void *) {
                GlyphRenderer renderer(GlyphRenderer::best());
                for (;;) {
                        (void)state->wake.acquire(WaitForever);
                        const uint32_t dirty = state->dirty.exchange(0, std::memory_order_acq_rel);
                        if ((dirty & Band) != 0) drawBand(renderer);
                        if ((dirty & Panel) != 0) {
                                drawPanel(renderer);
                        } else if ((dirty & Progress) != 0 &&
                                   state->phase.load(std::memory_order_acquire) == Phase::Running) {
                                drawProgress(renderer);
                        }
                        if ((dirty & Terminal) != 0) drawTerminal(renderer);
                }
        }

        void onKey(void *, const KeyEvent &event) {
                char         bytes[TerminalKeys::MaxBytes];
                const size_t n =
                        TerminalKeys::encode(event, state->applicationCursor.load(std::memory_order_relaxed), bytes);
                if (n > 0) (void)state->console.pushInput(ByteSpan(reinterpret_cast<const uint8_t *>(bytes), n));
                return;
        }

        void onStarted(void *, const Update::Report &report) {
                std::snprintf(state->version, sizeof(state->version), "%s", report.package.versionText);
                state->permille.store(0, std::memory_order_relaxed);
                state->phase.store(Phase::Running, std::memory_order_release);
                poke(Panel);
                return;
        }

        void onProgress(void *, Update::Stage stage, uint64_t done, uint64_t total) {
                const uint32_t      permille = total != 0 ? uint32_t(done * 1000 / total) : 0;
                const Update::Stage was = state->stage.exchange(stage, std::memory_order_relaxed);
                const uint32_t      last = state->permille.load(std::memory_order_relaxed);
                // A redraw every half percent at most: the job reports every 16 KB.
                if (was == stage && permille < last + 5 && permille != 1000) return;
                state->permille.store(permille, std::memory_order_relaxed);
                if (state->phase.load(std::memory_order_relaxed) == Phase::Running) poke(Progress);
                return;
        }

        void onFinished(void *, const Update::Report &report, const char *failure, bool restarting) {
                if (report.package.versionText[0] != '\0') {
                        std::snprintf(state->version, sizeof(state->version), "%s", report.package.versionText);
                }
                state->failure = failure;
                state->phase.store(failure == nullptr ? Phase::Applied
                                   : restarting       ? Phase::FailedRestarting
                                                      : Phase::FailedStaying,
                                   std::memory_order_release);
                poke(Panel);
                return;
        }

} // namespace

Error RecoveryScreen::init() {
        auto [display, derr] = Display::display(0);
        auto [mode, merr] = VideoModes::timing(Width, Height, 60);
        Error err = derr.isError() ? derr : merr;
        if (err.isOk()) err = display->claim("recovery");
        if (err.isOk()) err = display->start(mode, Surface::Buffering::Single);
        if (err.isError()) {
                bmdsWarn("Recovery: no screen (%s): the serial console only", err.desc());
                return Error::Ok;
        }
        void *memory = Memory::alloc(MemSpace::Internal, sizeof(State));
        auto *strip = static_cast<uint8_t *>(Memory::alloc(MemSpace::InternalDma, StripStride * CellHeight));
        auto *cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Psram, sizeof(TextCell) * Columns * Rows));
        if (memory == nullptr || strip == nullptr || cells == nullptr) {
                Memory::free(memory);
                Memory::free(strip);
                Memory::free(cells);
                display->stop();
                return Error::NoMem;
        }
        state = new (memory) State;
        state->surface = &display->surface();
        state->strip = strip;
        state->cells = cells;
        for (uint32_t &h : state->rowHash) h = ~0u; // every row drawn the first time
        err = state->console.resize(Columns, Rows);
        if (err.isError()) return err;
        (void)state->surface->fill(state->surface->bounds(), TerminalBackground, DrawTimeoutMs);
        state->console.setChangedHook([](void *) { poke(Terminal); }, nullptr);
        state->sink.key = onKey;
        Input::setSink(&state->sink);
        state->observer.started = onStarted;
        state->observer.progress = onProgress;
        state->observer.finished = onFinished;
        Update::setObserver(&state->observer);
        Thread::Config config;
        config.name = "screen";
        config.stackBytes = 8192;
        config.stackSpace = MemSpace::Internal; // it waits on the 2D-DMA
        config.priority = 3;
        config.kernel = true;
        err = state->drawer.start(config, drawMain, nullptr);
        if (err.isOk()) poke(Band | Panel | Terminal);
        return err;
}

Console *RecoveryScreen::console() {
        return state != nullptr ? &state->console : nullptr;
}

BMDS_NAMESPACE_END
