/**
 * @file      wincommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `win` command: the window manager on display 0, driven from the shell
 * until there is a mouse and a keyboard (devplan/windows.md W11, §4a). Its
 * output format is a userspace interface once shipped: keep the lines and
 * their order, add new ones only at the end.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <esp_cache.h>
#include <esp_cpu.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/keyboarddecoder.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/utf8.h>
#include <bmds/windowmanager.h>
#include <bmds/windowpalette.h>

#include "desktopmenu.h"
#include "wmconfig.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int SyncTimeoutMs = 1000;
        constexpr uint16_t     Square = 32; // the test pattern's squares
        constexpr int          DragSteps = 30;

        constexpr uint32_t LineColor = 0x202020; // the grid's 1-pixel lines
        constexpr uint32_t LineX = 5, LineY = 9; // where in each square (off the 4-pixel columns)
        constexpr uint16_t LabelX = 8, LabelY = 8;
        constexpr uint16_t Fixed = 32;      // rows at the top not scrolled (the label's)
        constexpr uint32_t Bias = 1u << 20; // keeps content rows positive (a multiple of Square)
        constexpr size_t   MaxLabel = 40;
        constexpr size_t   MaxRowCells = 240; // a text bench's row (1920 pixels)

        // A test window: a checker of two colors under a grid of 1-pixel lines,
        // scrolled by `offset` rows (`win scroll`) below a fixed band, and a
        // label of styled cells.
        struct TestWindow {
                        uint8_t              id = 0;
                        uint32_t             a = 0;
                        uint32_t             b = 0;
                        uint16_t             width = 0; // the client area's
                        uint16_t             height = 0;
                        int32_t              offset = 0;
                        std::atomic<int32_t> pending{0};       // rows to scroll at the next redraw
                        std::atomic<bool>    textBench{false}; // at the next redraw: the client area as text, timed
                        uint32_t             textUs = 0;
                        uint32_t             scrollUs = 0; // the last scroll
                        std::atomic<bool>    taken{false}; // a window's, or being opened
                        TestWindow          *next = nullptr;

                        // The pattern at (@p x, @p y) of the client area.
                        uint32_t at(uint32_t x, uint32_t y) const {
                                const uint32_t cy = y + Bias + uint32_t(offset);
                                if (x % Square == LineX || cy % Square == LineY) return LineColor;
                                return ((x / Square + cy / Square) & 1u) != 0 ? a : b;
                        }
        };

        constexpr uint32_t Palette[][2] = {
                {0xc04040, 0x803030}, {0x40c040, 0x308030}, {0x4060d0, 0x3040a0}, {0xc0c040, 0x909030},
                {0x40c0c0, 0x309090}, {0xc040c0, 0x903090}, {0xd08040, 0xa06030}, {0xa0a0a0, 0x707070},
        };

        // The test windows, in PSRAM, newest first: one closed is used again (they are never freed:
        // a window may be redrawn on the window manager's task while a command finds another).
        constinit std::atomic<TestWindow *> testWindows{nullptr};

        // Window @p id's test window, or null (not one).
        TestWindow *testWindow(uint8_t id) {
                for (TestWindow *t = testWindows.load(std::memory_order_acquire); t != nullptr; t = t->next)
                        if (t->taken.load(std::memory_order_acquire) && t->id == id) return t;
                return nullptr;
        }

        // A test window not in use, taken; or null (no memory).
        TestWindow *takeTestWindow() {
                for (TestWindow *t = testWindows.load(std::memory_order_acquire); t != nullptr; t = t->next) {
                        bool free = false;
                        if (t->taken.compare_exchange_strong(free, true, std::memory_order_acq_rel)) return t;
                }
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(TestWindow));
                if (mem == nullptr) return nullptr;
                auto *t = ::new (mem) TestWindow();
                t->taken.store(true, std::memory_order_relaxed);
                t->next = testWindows.load(std::memory_order_relaxed);
                while (!testWindows.compare_exchange_weak(t->next, t, std::memory_order_acq_rel)) {}
                return t;
        }

        // Window @p id's label: plain, bold, underlined and colored cells.
        size_t makeLabel(uint8_t id, TextCell (&out)[MaxLabel]) {
                char number[8];
                std::snprintf(number, sizeof(number), "%u ", id);
                struct Part {
                                const char *text;
                                uint32_t    fg, bg;
                                uint8_t     flags;
                };
                const Part parts[] = {
                        {"Win ", 0xffffff, 0x000000, TextCell::Bold},
                        {number, 0xffffff, 0x000000, 0},
                        {"under", 0xffff40, 0x2040a0, TextCell::Underline},
                        {" ░▒▓☺Ä ", 0x40ff40, 0x000000, 0},
                        {"both", 0xff8080, 0x202020, TextCell::Bold | TextCell::Underline},
                };
                size_t n = 0;
                for (const Part &part : parts) {
                        const StringView text(part.text);
                        for (size_t i = 0; i < text.size() && n < MaxLabel;) {
                                out[n].codepoint = Utf8::decode(text, i);
                                out[n].fg = part.fg;
                                out[n].bg = part.bg;
                                out[n].flags = part.flags;
                                ++n;
                        }
                }
                return n;
        }

        uint16_t labelWidth(const TestWindow &t) {
                return t.width > 2 * LabelX ? static_cast<uint16_t>(t.width - 2 * LabelX) : 0;
        }

        // The pattern over @p area (the client's coordinates): squares, then lines.
        void drawPattern(Painter &painter, const TestWindow &t, const Rect &area) {
                Rect   as[32], bs[32], lines[32];
                size_t na = 0, nb = 0, nl = 0;
                auto   flush = [&](Rect *list, size_t &n, uint32_t color) {
                        (void)painter.fill(Span<const Rect>(list, n), color);
                        n = 0;
                };
                const uint32_t top = area.y + Bias + uint32_t(t.offset), bottom = top + area.height;
                for (uint32_t cy = top / Square * Square; cy < bottom; cy += Square) {
                        const uint32_t y0 = cy > top ? cy : top, y1 = cy + Square < bottom ? cy + Square : bottom;
                        for (uint32_t x = area.x / Square * Square; x < area.right(); x += Square) {
                                const uint32_t x0 = x > area.x ? x : area.x;
                                const uint32_t x1 = x + Square < area.right() ? x + Square : area.right();
                                const Rect     r{uint16_t(x0), uint16_t(y0 - top + area.y), uint16_t(x1 - x0),
                                             uint16_t(y1 - y0)};
                                if (((x / Square + cy / Square) & 1u) != 0) {
                                        as[na++] = r;
                                        if (na == 32) flush(as, na, t.a);
                                } else {
                                        bs[nb++] = r;
                                        if (nb == 32) flush(bs, nb, t.b);
                                }
                        }
                }
                flush(as, na, t.a);
                flush(bs, nb, t.b);
                for (uint32_t x = area.x / Square * Square + LineX; x < area.right(); x += Square) {
                        if (x < area.x) continue;
                        lines[nl++] = Rect{uint16_t(x), area.y, 1, area.height};
                        if (nl == 32) flush(lines, nl, LineColor);
                }
                for (uint32_t cy = top / Square * Square + LineY; cy < bottom; cy += Square) {
                        if (cy < top) continue;
                        lines[nl++] = Rect{area.x, uint16_t(cy - top + area.y), area.width, 1};
                        if (nl == 32) flush(lines, nl, LineColor);
                }
                flush(lines, nl, LineColor);
                return;
        }

        // A pending scroll made (the band under the label moved, the rest
        // redrawn), the damage redrawn, the label drawn over it.
        void redrawTest(void *arg, Painter &painter) {
                TestWindow   &t = *static_cast<TestWindow *>(arg);
                Region        todo;
                const int32_t dy = t.pending.exchange(0);
                if (dy != 0 && t.height > Fixed) {
                        const uint64_t start = System::uptimeUs();
                        t.offset += dy;
                        todo.add(painter.scroll(Rect{0, Fixed, t.width, uint16_t(t.height - Fixed)}, -dy));
                        todo.add(Rect{0, 0, t.width, Fixed});
                        t.scrollUs = static_cast<uint32_t>(System::uptimeUs() - start);
                }
                for (size_t i = 0; i < painter.damageCount(); ++i) todo.add(painter.damageAt(i));
                for (const Rect &r : todo.rects()) drawPattern(painter, t, r);
                if (t.textBench.exchange(false)) {
                        // A terminal's full redraw: rows of cells across the client area.
                        const uint64_t start = System::uptimeUs();
                        auto          *row = static_cast<TextCell *>(
                                Memory::alloc(MemSpace::Internal, sizeof(TextCell) * MaxRowCells));
                        if (row == nullptr) return;
                        const size_t columns = t.width / 8u < MaxRowCells ? t.width / 8u : MaxRowCells;
                        for (uint16_t y = 0; y + 16u <= t.height; y = static_cast<uint16_t>(y + 16)) {
                                for (size_t c = 0; c < columns; ++c) {
                                        row[c].codepoint = 0x21 + (c + y / 16u) % 94;
                                        row[c].fg = 0xc0c0c0;
                                        row[c].bg = 0x000000;
                                }
                                (void)painter.cells(0, y, Span<const TextCell>(row, columns), Fonts::unscii16, t.width);
                        }
                        t.textUs = static_cast<uint32_t>(System::uptimeUs() - start);
                        Memory::free(row);
                        return;
                }
                TextCell     label[MaxLabel];
                const size_t n = makeLabel(t.id, label);
                (void)painter.cells(LabelX, LabelY, Span<const TextCell>(label, n), Fonts::unscii16, labelWidth(t));
                return;
        }

        // A test window's new client size.
        void resizedTest(void *arg, uint16_t width, uint16_t height) {
                TestWindow &t = *static_cast<TestWindow *>(arg);
                t.width = width;
                t.height = height;
                return;
        }

        // A test window closed: its number no longer a test window's.
        void closedTest(void *arg) {
                auto *t = static_cast<TestWindow *>(arg);
                t->id = 0;
                t->taken.store(false, std::memory_order_release);
                return;
        }

        // Display 0's window manager if it runs, or null with a message.
        WindowManager *running(CommandContext &ctx) {
                auto [wm, err] = WindowManager::manager(0);
                if (err.isError() || !wm->isRunning()) {
                        ctx.eprintf("win: not started ('win start')\n");
                        return nullptr;
                }
                return wm;
        }

        bool parseNumber(const char *text, long max, long &out) {
                char *end = nullptr;
                out = std::strtol(text, &end, 0);
                return end != text && *end == '\0' && out >= 0 && out <= max;
        }

        int report(CommandContext &ctx, Error err) {
                if (err.isOk()) return Shell::ExitSuccess;
                ctx.eprintf("win: %s\n", err.desc());
                return Shell::ExitFailure;
        }

        // win start: the palette is the system configuration's wm/palette (devplan/sysconf.md §8).
        int runStart(CommandContext &ctx) {
                auto [d, derr] = Display::display(0);
                if (derr.isError() || !d->isRunning()) {
                        ctx.eprintf("win: the display isn't running ('display start')\n");
                        return Shell::ExitFailure;
                }
                const WindowPalette palette = WmConfig::palette();
                auto [wm, err] = WindowManager::manager(0);
                if (err.isOk()) err = wm->start(*d, palette);
                if (err.isOk()) (void)WmConfig::follow(*wm);
                if (err == Error::Busy) {
                        ctx.eprintf("win: display in use by the %s\n", d->owner());
                        return Shell::ExitFailure;
                }
                // The desktop's own menu (devplan/gui.md §4o): a terminal without any program's help.
                if (err.isOk() && installDesktopMenu(*wm, ctx).isError())
                        ctx.eprintf("win: no desktop menu (out of memory)\n");
                return report(ctx, err);
        }

        int runList(CommandContext &ctx, WindowManager &wm) {
                auto [info, lerr] = wm.windows();
                if (lerr.isError()) return report(ctx, lerr);
                const size_t n = info.size();
                ctx.printf("Win  X     Y     W     H     Title\n");
                for (size_t i = 0; i < n; ++i) {
                        const WindowManager::Info &w = info[i];

                        ctx.printf("%-4u %-5u %-5u %-5u %-5u %s%s%s%s%s%s%s%s\n", w.id, w.rect.x, w.rect.y,
                                   w.rect.width, w.rect.height, w.title, w.focused ? " (focus)" : "",
                                   w.kind[0] != '\0' ? " (" : "", w.kind, w.kind[0] != '\0' ? ")" : "",
                                   w.minimized   ? " (minimized)"
                                   : w.maximized ? " (maximized)"
                                                 : "",
                                   w.level == WindowStack::Level::Above ? " (above)" : "", w.shaded ? " (shaded)" : "");
                }
                return Shell::ExitSuccess;
        }

        // win new [X Y W H] [TITLE...]
        int runNew(CommandContext &ctx, WindowManager &wm, Args args) {
                auto [info, lerr] = wm.windows();
                if (lerr.isError()) return report(ctx, lerr);
                const size_t                open = info.size();
                WindowManager::WindowConfig config;
                config.rect = Rect{uint16_t(64 + 48 * open), uint16_t(48 + 36 * open), 400, 300};
                size_t next = 2;
                if (args.size() >= 6) {
                        long v[4];
                        for (int i = 0; i < 4; ++i)
                                if (!parseNumber(args[2 + i], UINT16_MAX, v[i])) return ctx.usage();
                        config.rect = Rect{uint16_t(v[0]), uint16_t(v[1]), uint16_t(v[2]), uint16_t(v[3])};
                        next = 6;
                }
                char title[WindowManager::MaxTitle + 1] = {};
                for (size_t i = next, used = 0; i < args.size(); ++i) {
                        const int wrote =
                                std::snprintf(title + used, sizeof(title) - used, "%s%s", used > 0 ? " " : "", args[i]);
                        if (wrote < 0) break;
                        used += size_t(wrote);
                        if (used >= sizeof(title)) break;
                }
                config.rect.width = static_cast<uint16_t>((config.rect.width + 3) & ~3u); // as it will be placed
                config.redraw = redrawTest;
                config.resized = resizedTest;
                config.closed = closedTest;
                TestWindow *tw = takeTestWindow();
                if (tw == nullptr) return report(ctx, Error::NoMem);
                TestWindow &t = *tw;
                // Its number, colors and title once it has its number: drawn again then.
                t.id = 0;
                t.a = t.b = 0;
                t.offset = 0;
                t.pending.store(0);
                const Rect client = WindowFrame(config.rect, config.decorations).client();
                t.width = client.width; // until the window is placed (below)
                t.height = client.height;
                config.arg = &t;
                config.title = title;
                auto [id, err] = wm.open(config);
                if (err.isError()) {
                        t.taken.store(false, std::memory_order_release);
                        return report(ctx, err);
                }
                t.a = Palette[(id - 1u) % (sizeof(Palette) / sizeof(Palette[0]))][0];
                t.b = Palette[(id - 1u) % (sizeof(Palette) / sizeof(Palette[0]))][1];
                t.id = id;
                auto [placed, perr] = wm.clientRect(id);
                if (perr.isOk()) {
                        t.width = placed.width;
                        t.height = placed.height;
                }
                if (title[0] == '\0') {
                        std::snprintf(title, sizeof(title), "Window %u", id);
                        (void)wm.setTitle(id, StringView(title));
                }
                (void)wm.update(id);
                ctx.printf("win %u\n", id);
                return Shell::ExitSuccess;
        }

        bool parseId(CommandContext &ctx, const char *text, uint8_t &id) {
                long v = 0;
                if (!parseNumber(text, WindowStack::MaxWindows, v) || v == 0) {
                        ctx.eprintf("win: %s: not a window number\n", text);
                        return false;
                }
                id = static_cast<uint8_t>(v);
                return true;
        }

        // win drag N X Y: the outline walked there over DragSteps frames, then dropped.
        int runDrag(CommandContext &ctx, WindowManager &wm, uint8_t id, long x, long y) {
                auto [info, lerr] = wm.windows();
                if (lerr.isError()) return report(ctx, lerr);
                const size_t n = info.size();
                const Rect  *from = nullptr;
                for (size_t i = 0; i < n; ++i)
                        if (info[i].id == id) from = &info[i].rect;
                if (from == nullptr) return report(ctx, Error::NotFound);
                const uint64_t start = System::uptimeUs();
                for (int step = 1; step <= DragSteps && !ctx.isInterrupted(); ++step) {
                        const long  px = from->x + (x - from->x) * step / DragSteps;
                        const long  py = from->y + (y - from->y) * step / DragSteps;
                        const Error err = wm.drag(id, uint16_t(px), uint16_t(py));
                        if (err.isError()) return report(ctx, err);
                        (void)wm.sync(SyncTimeoutMs);
                }
                const uint64_t dragged = System::uptimeUs() - start;
                Error          err = wm.drop();
                if (err.isOk()) err = wm.sync(SyncTimeoutMs);
                if (err.isError()) return report(ctx, err);
                ctx.printf("drag:     %d steps in %llu ms, dropped in %llu ms\n", DragSteps,
                           static_cast<unsigned long long>(dragged / 1000),
                           static_cast<unsigned long long>((System::uptimeUs() - start - dragged) / 1000));
                return Shell::ExitSuccess;
        }

        bool inside(const Rect &r, uint32_t x, uint32_t y) {
                return x >= r.x && x < r.right() && y >= r.y && y < r.bottom();
        }

        // What @p text written at (@p left, @p top), @p width at most, shows
        // at (@p x, @p y) of its row: @p fg on its glyphs' bits, else @p bg.
        uint32_t textAt(StringView text, uint32_t left, uint32_t top, uint32_t width, uint32_t x, uint32_t y,
                        uint32_t fg, uint32_t bg) {
                const Font    &font = Fonts::unscii16;
                const uint32_t end = left + width;
                for (size_t i = 0; i < text.size();) {
                        const Glyph g = font.glyph(Utf8::decode(text, i));
                        if (left + g.width > end) break;
                        if (x >= left && x < left + g.width) {
                                const uint32_t col = x - left;
                                const uint8_t  bits = g.rows[(y - top) * (g.width / 8u) + col / 8];
                                return (bits & (0x80u >> (col % 8))) != 0 ? fg : bg;
                        }
                        left += g.width;
                }
                return bg;
        }

        // What window @p w's frame shows at (@p x, @p y): its border, title
        // bar, title, and buttons (none pressed).
        uint32_t frameAt(const WindowPalette &palette, const WindowManager::Info &w, const Icon &icon, bool canMinimize,
                         uint32_t x, uint32_t y) {
                using Role = WindowPalette::Role;
                using Part = WindowFrame::Part;
                const WindowFrame f(w.rect, w.decorations);
                const uint32_t    frame = palette.color(w.focused ? Role::ActiveFrame : Role::InactiveFrame);
                const uint32_t    text = palette.color(w.focused ? Role::ActiveTitle : Role::InactiveTitle);
                struct Button {
                                Part     part;
                                Role     face, mark;
                                uint32_t icon;
                };
                const Button buttons[] = {
                        {Part::Menu, Role::Button, Role::ButtonMark, IconGlyph::Menu},
                        {Part::Minimize, Role::Button, Role::ButtonMark, IconGlyph::Minimize},
                        {Part::Maximize, Role::Button, Role::ButtonMark,
                         w.maximized ? IconGlyph::Restore : IconGlyph::Maximize},
                        {Part::Close, Role::CloseBox, Role::CloseMark, IconGlyph::Close},
                };
                for (const Button &b : buttons) {
                        const Rect r = f.button(b.part);
                        if (!inside(r, x, y)) continue;
                        if (b.part == Part::Menu && icon.isValid() && icon.width <= r.width &&
                            icon.height <= r.height) {
                                // The window's icon in the menu button's place, over the title bar.
                                if (x - r.x >= icon.width || y - r.y >= icon.height) return frame;
                                return blendOver(icon.pixels[(y - r.y) * icon.width + (x - r.x)], frame);
                        }
                        Bevel::Edge  edges[Bevel::MaxEdges];
                        const size_t n = Bevel::edges(r, Bevel::Style::Raised, edges);
                        for (size_t i = 0; i < n; ++i)
                                if (inside(edges[i].rect, x, y)) return palette.color(edges[i].role);
                        const Glyph    g = Fonts::icons16.glyph(b.icon);
                        const uint32_t col = x - r.x;
                        const bool     on = (g.rows[(y - r.y) * 2 + col / 8] & (0x80u >> (col % 8))) != 0;
                        // The minimize button's mark disabled with nowhere to minimize to.
                        const Role mark = b.part == Part::Minimize && !canMinimize ? Role::DisabledText : b.mark;
                        return palette.color(on ? mark : b.face);
                }
                const Rect t = f.title();
                if (inside(t, x, y)) return textAt(StringView(w.title), t.x, t.y, t.width, x, y, text, frame);
                return frame;
        }

        // Every pixel of the shown picture against the stack: the desktop's
        // color, a frame (its title's glyphs too), or a test window's pattern.
        int runCheck(CommandContext &ctx, WindowManager &wm) {
                Error err = wm.sync(SyncTimeoutMs);
                if (err.isError()) return report(ctx, err);
                auto [d, derr] = Display::display(0);
                if (derr.isError()) return report(ctx, derr);
                const Surface &s = d->surface();
                auto [info, lerr] = wm.windows();
                if (lerr.isError()) return report(ctx, lerr);
                const size_t n = info.size();
                // Each window's client area, label, icon, reference picture and test window.
                struct Checked {
                                Rect              client;
                                Rect              label;
                                Icon              icon;
                                uint8_t          *image = nullptr;
                                const TestWindow *test = nullptr;
                };
                List<Checked> checked(MemSpace::Psram);
                if (checked.reserve(n).isError()) return report(ctx, Error::NoMem);
                for (size_t i = 0; i < n; ++i) {
                        Checked c;
                        c.client = WindowFrame(info[i].rect, info[i].decorations).client();
                        c.test = testWindow(info[i].id);
                        (void)checked.append(c); // reserved
                }
                const uint8_t *buffer = s.shown();
                const size_t   bytes = s.stride() * s.height();
                for (size_t off = 0; off < bytes; off += 64 * 1024)
                        (void)esp_cache_msync(const_cast<uint8_t *>(buffer) + off,
                                              bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                // Each test window's label, drawn by the scalar kernel (a reference for the painter's).
                constexpr uint16_t LabelPixels = MaxLabel * 16;
                const size_t       labelBytes = size_t(LabelPixels) * 3 * Fonts::unscii16.height();
                auto *labels = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, labelBytes * (n > 0 ? n : 1)));
                if (labels == nullptr) return report(ctx, Error::NoMem);
                GlyphRenderer reference(GlyphRenderer::Kernel::Scalar);
                for (size_t i = 0; i < n; ++i) {
                        const TestWindow *t = checked[i].test;
                        if (t == nullptr) continue;
                        TextCell       cells[MaxLabel];
                        const size_t   count = makeLabel(t->id, cells);
                        size_t         used = 0;
                        const uint16_t w = reference.drawCells(Span<const TextCell>(cells, count), Fonts::unscii16,
                                                               labels + i * labelBytes, size_t(LabelPixels) * 3,
                                                               labelWidth(*t), used);
                        checked[i].label = Rect{uint16_t(checked[i].client.x + LabelX),
                                                uint16_t(checked[i].client.y + LabelY), w, Fonts::unscii16.height()};
                }
                const WindowPalette palette = wm.palette();
                const bool          canMinimize = wm.canMinimize();
                // Each window's icon (the title bar shows a 16 x 16 one).
                constexpr size_t IconPixels = 16 * 16;
                auto            *iconPixels = static_cast<uint32_t *>(
                        Memory::alloc(MemSpace::Psram, sizeof(uint32_t) * IconPixels * (n > 0 ? n : 1)));
                for (size_t i = 0; i < n && iconPixels != nullptr; ++i) {
                        auto [icon, ierr] =
                                wm.copyIcon(info[i].id, Span<uint32_t>(iconPixels + i * IconPixels, IconPixels));
                        if (ierr.isOk()) checked[i].icon = icon;
                }
                const auto freeAll = [&]() {
                        for (Checked &c : checked) Memory::free(c.image);
                        Memory::free(labels);
                        Memory::free(iconPixels);
                };
                // Each window that offers it: its client area as it should be (a terminal's, from its screen).
                for (size_t i = 0; i < n; ++i) {
                        if (info[i].minimized || checked[i].client.isEmpty()) continue; // a rolled-up one shows none
                        const Rect  &c = checked[i].client;
                        const size_t stride = size_t(c.width) * 3;
                        uint8_t     *image = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, stride * c.height));
                        if (image == nullptr) {
                                freeAll();
                                return report(ctx, Error::NoMem);
                        }
                        if (wm.reference(info[i].id, image, stride, c.width, c.height).isError()) {
                                Memory::free(image); // a test window: its pattern, below
                                image = nullptr;
                        }
                        checked[i].image = image;
                }
                const uint32_t desktop = palette.color(WindowPalette::Role::Desktop);
                const Rect     pointer = wm.pointerArea(); // over everything: not modelled
                uint32_t       wrong = 0, compared = 0, unknown = 0;
                int            fx = -1, fy = -1;
                uint32_t       fgot = 0, fwant = 0;
                for (uint32_t y = 0; y < s.height(); ++y) {
                        const uint8_t *line = buffer + size_t(y) * s.stride();
                        for (uint32_t x = 0; x < s.width(); ++x) {
                                if (inside(pointer, x, y)) continue;
                                uint32_t want = desktop;
                                bool     known = true;
                                for (size_t i = 0; i < n; ++i) {
                                        if (info[i].minimized || !inside(info[i].rect, x, y)) continue;
                                        const Checked &w = checked[i];
                                        const Rect    &c = w.client;
                                        if (inside(c, x, y) && w.image == nullptr && w.test == nullptr) {
                                                known = false; // a client area offering no reference (a menu)
                                        } else if (!inside(c, x, y)) {
                                                want = frameAt(palette, info[i], w.icon, canMinimize, x, y);
                                        } else if (w.image != nullptr) {
                                                const uint8_t *q =
                                                        w.image + (size_t(y - c.y) * c.width + (x - c.x)) * 3;
                                                want = uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
                                        } else if (inside(w.label, x, y)) {
                                                const uint8_t *q = labels + i * labelBytes +
                                                                   (y - w.label.y) * size_t(LabelPixels) * 3 +
                                                                   (x - w.label.x) * 3;
                                                want = uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
                                        } else {
                                                want = w.test->at(x - c.x, y - c.y);
                                        }
                                        break;
                                }
                                if (!known) {
                                        ++unknown;
                                        continue;
                                }
                                ++compared;
                                const uint8_t *p = line + 3 * x;
                                const uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                if (got == want) continue;
                                if (wrong++ == 0) {
                                        fx = int(x);
                                        fy = int(y);
                                        fgot = got;
                                        fwant = want;
                                }
                        }
                }
                ctx.printf("check:    %lu of %lu pixels wrong", static_cast<unsigned long>(wrong),
                           static_cast<unsigned long>(compared));
                if (unknown > 0)
                        ctx.printf(" (%lu not checked: windows without a reference)",
                                   static_cast<unsigned long>(unknown));
                if (wrong > 0)
                        ctx.printf(" (the first at %d,%d: %06lx, not %06lx)", fx, fy, static_cast<unsigned long>(fgot),
                                   static_cast<unsigned long>(fwant));
                ctx.printf("\n");
                freeAll();
                return wrong == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        // win shuffle N: N random raises and moves, each drawn before the next.
        int runShuffle(CommandContext &ctx, WindowManager &wm, long count) {
                uint32_t state = static_cast<uint32_t>(System::uptimeUs());
                auto     next = [&state](uint32_t limit) {
                        state = state * 1103515245u + 12345u;
                        return (state >> 8) % limit;
                };
                const uint64_t start = System::uptimeUs();
                uint64_t       longest = 0;
                long           done = 0;
                for (; done < count && !ctx.isInterrupted(); ++done) {
                        auto [info, lerr] = wm.windows();
                        if (lerr.isError()) return report(ctx, lerr);
                        const size_t n = info.size();
                        if (n == 0) break;
                        const WindowManager::Info &w = info[next(uint32_t(n))];
                        const uint64_t             t = System::uptimeUs();
                        Error                      err;
                        if (next(2) == 0) {
                                err = wm.raise(w.id);
                        } else {
                                auto [d, derr] = Display::display(0);
                                if (derr.isError()) return report(ctx, derr);
                                const Surface &s = d->surface();
                                err = wm.move(w.id, uint16_t(next(uint32_t(s.width() - w.rect.width + 1))),
                                              uint16_t(next(uint32_t(s.height() - w.rect.height + 1))));
                        }
                        if (err.isOk()) err = wm.sync(SyncTimeoutMs);
                        if (err.isError()) return report(ctx, err);
                        const uint64_t us = System::uptimeUs() - t;
                        if (us > longest) longest = us;
                }
                const uint64_t us = System::uptimeUs() - start;
                ctx.printf("shuffle:  %ld changes in %llu ms, %llu us each on average, %llu at most\n", done,
                           static_cast<unsigned long long>(us / 1000),
                           static_cast<unsigned long long>(done > 0 ? us / uint64_t(done) : 0),
                           static_cast<unsigned long long>(longest));
                return Shell::ExitSuccess;
        }

        // win bench: the glyph kernels on a 1280-pixel row of cells, timed and
        // compared byte for byte.
        int runBench(CommandContext &ctx, unsigned Cells) {
                constexpr unsigned MaxCells = 160, Rounds = 50;
                if (Cells == 0 || Cells > MaxCells) Cells = MaxCells;
                const unsigned Width = Cells * 8;
                const Font    &font = Fonts::unscii16;
                const size_t   stride = size_t(Width) * 3, bytes = stride * font.height();
                uint8_t       *strips[2] = {};
                for (uint8_t *&strip : strips)
                        strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Internal, bytes, 64));
                if (strips[0] == nullptr || strips[1] == nullptr) {
                        Memory::free(strips[0]);
                        Memory::free(strips[1]);
                        return report(ctx, Error::NoMem);
                }
                // A terminal-like row: text, a color change every 7 cells, some styled.
                // In internal RAM: the shell's stack is in PSRAM.
                struct Work {
                                TextCell      cells[MaxCells];
                                GlyphRenderer renderers[2] = {GlyphRenderer(GlyphRenderer::Kernel::Scalar),
                                                              GlyphRenderer(GlyphRenderer::Kernel::Pie)};
                };
                void *workMemory = Memory::allocAligned(MemSpace::Internal, sizeof(Work), 16);
                if (workMemory == nullptr) {
                        Memory::free(strips[0]);
                        Memory::free(strips[1]);
                        return report(ctx, Error::NoMem);
                }
                Work     *work = new (workMemory) Work();
                TextCell *cells = work->cells;
                for (unsigned i = 0; i < Cells; ++i) {
                        cells[i].codepoint = 0x21 + (i * 7) % 94;
                        cells[i].fg = (i / 7) % 2 != 0 ? 0xffff40 : 0xc0c0c0;
                        cells[i].bg = (i / 7) % 3 != 0 ? 0x000000 : 0x202080;
                        cells[i].flags = i % 11 == 0 ? TextCell::Bold : (i % 13 == 0 ? TextCell::Underline : 0);
                }
                const GlyphRenderer::Kernel kernels[2] = {GlyphRenderer::Kernel::Scalar, GlyphRenderer::Kernel::Pie};
                uint32_t                    cycles[2] = {};
                for (int k = 0; k < 2; ++k) {
                        GlyphRenderer &renderer = work->renderers[k];
                        std::memset(strips[k], 0, bytes);
                        const uint32_t start = esp_cpu_get_cycle_count();
                        size_t         used = 0;
                        for (unsigned r = 0; r < Rounds; ++r)
                                (void)renderer.drawCells(Span<const TextCell>(cells, Cells), font, strips[k], stride,
                                                         uint16_t(Width), used);
                        cycles[k] = esp_cpu_get_cycle_count() - start;
                }
                const bool same = std::memcmp(strips[0], strips[1], bytes) == 0;
                for (int k = 0; k < 2; ++k)
                        ctx.printf("%-8s %lu cycles a cell (%lu us a 1280-pixel row)%s\n", k == 0 ? "scalar:" : "pie:",
                                   static_cast<unsigned long>(cycles[k] / (Cells * Rounds)),
                                   static_cast<unsigned long>(cycles[k] / (Cells * Rounds) * 160 / 360),
                                   GlyphRenderer::isSupported(kernels[k]) ? "" : " (not in this build: scalar)");
                ctx.printf("compare:  %s\n", same ? "the same bytes" : "DIFFERENT");
                Memory::free(strips[0]);
                Memory::free(strips[1]);
                Memory::free(workMemory);
                return same ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        // win term [N]: /bin/terminal in a window, started and not waited for (the Nth cascaded:
        // its top-left corner at 32 + 32 N); its window's number and size when it shows.
        int runTerm(CommandContext &ctx, WindowManager &wm, Args args) {
                long number = 0;
                if (args.size() == 3 && !parseNumber(args[2], 15, number)) return ctx.usage();
                if (ctx.shell().launcher() == nullptr) return report(ctx, Error::NotSupported);
                auto [before, berr] = wm.windows();
                if (berr.isError()) return report(ctx, berr);
                char                at[8];
                std::snprintf(at, sizeof(at), "%ld", 32 + 32 * number);
                const StringView argv[] = {"terminal", at, at};
                List<String>     env;
                List<StringView> envViews;
                Error            err;
                for (const Variables::Entry &e : ctx.executor().state().vars.entries()) {
                        if ((e.flags & Variables::Exported) == 0 || (e.flags & Variables::IsSet) == 0) continue;
                        String line;
                        err = line.append(e.name);
                        if (err.isOk()) err = line.append('=');
                        if (err.isOk()) err = line.append(e.value);
                        if (err.isOk()) err = env.append(std::move(line));
                        if (err.isError()) return report(ctx, err);
                }
                if (envViews.reserve(env.size()).isError()) return report(ctx, Error::NoMem);
                for (const String &e : env) (void)envViews.append(e.view());
                ProgramLauncher::Request request;
                request.path = "/bin/terminal";
                request.args = Span<const StringView>(argv);
                request.environment = Span<const StringView>(envViews.data(), envViews.size());
                request.fds = &ctx.fds();
                request.cwd = ctx.cwd();
                request.console = &ctx.shell().console();
                auto [pid, serr] = ctx.shell().launcher()->start(request);
                if (serr.isError()) return report(ctx, serr);
                // Its window, once it shows (up to 2 s).
                for (int tries = 0; tries < 200; ++tries) {
                        auto [now, nerr] = wm.windows();
                        const size_t n = nerr.isOk() ? now.size() : 0;
                        for (size_t i = 0; i < n; ++i) {
                                bool old = false;
                                for (const WindowManager::Info &b : before) old = old || b.id == now[i].id;
                                if (old) continue;
                                auto [client, cerr] = wm.clientRect(now[i].id);
                                if (cerr.isError()) continue;
                                ctx.printf("win %u: terminal, %ux%u\n", now[i].id, client.width / 8u,
                                           client.height / 16u);
                                return Shell::ExitSuccess;
                        }
                        Task::sleep(10);
                }
                ctx.eprintf("win: terminal started (process %ld) but no window showed\n", static_cast<long>(pid));
                return Shell::ExitFailure;
        }

        // The bytes of args[from...], joined by spaces: C escapes (\\n \\r \\t \\e \\xHH) and ^X controls.
        size_t typed(Args args, size_t from, uint8_t (&buf)[256]) {
                size_t n = 0;
                for (size_t a = from; a < args.size(); ++a) {
                        if (a > from && n < sizeof(buf)) buf[n++] = ' ';
                        for (const char *p = args[a]; *p != '\0' && n < sizeof(buf); ++p) {
                                char c = *p;
                                if (c == '^' && p[1] >= '@' && p[1] <= '_') {
                                        c = static_cast<char>(*++p - '@');
                                } else if (c == '\\' && p[1] != '\0') {
                                        switch (*++p) {
                                                case 'n': c = '\n'; break;
                                                case 'r': c = '\r'; break;
                                                case 't': c = '\t'; break;
                                                case 'e': c = '\x1b'; break;
                                                case 'x': {
                                                        char       hex[3] = {p[1], p[1] != '\0' ? p[2] : '\0', '\0'};
                                                        char      *end = nullptr;
                                                        const long v = std::strtol(hex, &end, 16);
                                                        p += end - hex;
                                                        c = static_cast<char>(v);
                                                        break;
                                                }
                                                default: c = *p; break;
                                        }
                                }
                                buf[n++] = static_cast<uint8_t>(c);
                        }
                }
                return n;
        }

        // The key a US keyboard types @p c with: its usage and modifiers. False: none does.
        bool keyFor(uint8_t c, uint8_t &usage, uint8_t &modifiers) {
                modifiers = 0;
                switch (c) {
                        case '\r':
                        case '\n': usage = HidKey::Enter; return true;
                        case '\t': usage = HidKey::Tab; return true;
                        case 0x1b: usage = HidKey::Escape; return true;
                        case 0x7f: usage = HidKey::Backspace; return true;
                        default: break;
                }
                if (c >= 1 && c <= 26) { // ^A..^Z
                        usage = static_cast<uint8_t>(HidKey::A + c - 1);
                        modifiers = KeyEvent::LeftCtrl;
                        return true;
                }
                for (const uint8_t mods : {uint8_t(0), uint8_t(KeyEvent::LeftShift)})
                        for (uint8_t u = HidKey::A; u <= HidKey::NonUsBackslash; ++u)
                                if (Keymap::us(u, mods, 0) == c) {
                                        usage = u;
                                        modifiers = mods;
                                        return true;
                                }
                return false;
        }

        // Keys for @p text's bytes, as a keyboard would send them, to window @p id (0: the focus).
        int sendKeys(CommandContext &ctx, WindowManager &wm, Args args, size_t from, uint8_t id) {
                uint8_t      buf[256];
                const size_t n = typed(args, from, buf);
                for (size_t i = 0; i < n; ++i) {
                        KeyEvent e;
                        if (!keyFor(buf[i], e.usage, e.modifiers)) {
                                ctx.eprintf("win: no key types 0x%02x\n", buf[i]);
                                return Shell::ExitFailure;
                        }
                        e.codepoint = Keymap::us(e.usage, e.modifiers, 0);
                        e.action = KeyEvent::Action::Press;
                        wm.keyTo(id, e);
                        e.action = KeyEvent::Action::Release;
                        e.codepoint = 0;
                        wm.keyTo(id, e);
                        // Delivered a few at a time: the queue holds KeyQueue events.
                        if (i % 8 == 7) {
                                const Error err = wm.sync(SyncTimeoutMs);
                                if (err.isError()) return report(ctx, err);
                        }
                }
                return report(ctx, wm.sync(SyncTimeoutMs));
        }

        // win key TEXT...: typed as a keyboard would, through the window manager (to the focus).
        int runKey(CommandContext &ctx, WindowManager &wm, Args args) {
                return sendKeys(ctx, wm, args, 2, 0);
        }

        // win type N TEXT...: typed into window N, as keys; C escapes and ^X controls.
        int runType(CommandContext &ctx, WindowManager &wm, Args args) {
                uint8_t id = 0;
                if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                return sendKeys(ctx, wm, args, 3, id);
        }

        // win dump N: window N's contents as text (a terminal: its screen, the cursor's place).
        int runDump(CommandContext &ctx, WindowManager &wm, const char *text) {
                uint8_t id = 0;
                if (!parseId(ctx, text, id)) return Shell::ExitUsage;
                FileHandle *out = ctx.fds().handle(FdTable::Stdout);
                if (out == nullptr) return report(ctx, Error::InvalidState);
                const Error err = wm.dump(id, out->stream());
                if (err == Error::NotSupported) {
                        ctx.eprintf("win: %u: has no text\n", id);
                        return Shell::ExitFailure;
                }
                return report(ctx, err);
        }

        // The decorations' names, as `win decorate` takes and prints them.
        struct DecorationName {
                        const char *name;
                        uint8_t     flag;
        };
        constexpr DecorationName DecorationNames[] = {
                {"border", WindowFrame::Border},           {"title", WindowFrame::TitleBar},
                {"menu", WindowFrame::MenuButton},         {"minimize", WindowFrame::MinimizeButton},
                {"maximize", WindowFrame::MaximizeButton}, {"close", WindowFrame::CloseButton},
                {"resize", WindowFrame::Resizable},
        };

        // win decorate N [all|none|+NAME|-NAME...]: changed in order, then printed.
        int runDecorate(CommandContext &ctx, WindowManager &wm, Args args) {
                uint8_t id = 0;
                if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                auto [info, lerr] = wm.windows();
                if (lerr.isError()) return report(ctx, lerr);
                const size_t               n = info.size();
                const WindowManager::Info *w = nullptr;
                for (size_t i = 0; i < n; ++i)
                        if (info[i].id == id) w = &info[i];
                if (w == nullptr) return report(ctx, Error::NotFound);
                uint8_t flags = w->decorations;
                for (size_t a = 3; a < args.size(); ++a) {
                        const char *arg = args[a];
                        if (std::strcmp(arg, "all") == 0) {
                                flags = WindowFrame::Standard;
                                continue;
                        }
                        if (std::strcmp(arg, "none") == 0) {
                                flags = WindowFrame::None;
                                continue;
                        }
                        uint8_t flag = 0;
                        for (const DecorationName &d : DecorationNames)
                                if ((arg[0] == '+' || arg[0] == '-') && std::strcmp(arg + 1, d.name) == 0)
                                        flag = d.flag;
                        if (flag == 0) {
                                ctx.eprintf("win: %s: not a decoration (all, none, +NAME or -NAME: border, title, "
                                            "menu, minimize, maximize, close, resize)\n",
                                            arg);
                                return Shell::ExitUsage;
                        }
                        flags = static_cast<uint8_t>(arg[0] == '+' ? flags | flag : flags & ~flag);
                }
                if (args.size() > 3) {
                        const Error err = wm.setDecorations(id, flags);
                        if (err == Error::Invalid) {
                                ctx.eprintf("win: buttons need the title, resize the border; or too small a "
                                            "window for them\n");
                                return Shell::ExitFailure;
                        }
                        if (err.isError()) return report(ctx, err);
                }
                ctx.printf("decorations:");
                for (const DecorationName &d : DecorationNames)
                        if ((flags & d.flag) != 0) ctx.printf(" %s", d.name);
                ctx.printf("%s\n", flags == WindowFrame::None ? " none" : "");
                return Shell::ExitSuccess;
        }

        int runStats(CommandContext &ctx, WindowManager &wm) {
                const WindowManager::Stats s = wm.stats();
                ctx.printf("Rounds:   %lu (%lu pieces, %lu redraws, %lu DMA calls, %lu fallbacks, %lu errors)\n",
                           static_cast<unsigned long>(s.rounds), static_cast<unsigned long>(s.pieces),
                           static_cast<unsigned long>(s.redraws), static_cast<unsigned long>(s.calls),
                           static_cast<unsigned long>(s.fallbacks), static_cast<unsigned long>(s.errors));
                ctx.printf("Drawing:  %llu us a round on average, %lu at most, %lu the last\n",
                           static_cast<unsigned long long>(s.rounds > 0 ? s.roundUs / s.rounds : 0),
                           static_cast<unsigned long>(s.roundUsMax), static_cast<unsigned long>(s.lastRoundUs));
                ctx.printf("Deferred: %lu redraws (their windows' owners busy)\n",
                           static_cast<unsigned long>(s.deferred));
                return Shell::ExitSuccess;
        }

        int runWin(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 2 && std::strcmp(args[1], "start") == 0) return runStart(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "stop") == 0) {
                        auto [wm, err] = WindowManager::manager(0);
                        if (err.isOk()) wm->stop();
                        return Shell::ExitSuccess;
                }
                if (args.size() >= 2 && std::strcmp(args[1], "bench") == 0)
                        return runBench(ctx, args.size() > 2 ? unsigned(std::atoi(args[2])) : 0);
                WindowManager *wm = running(ctx);
                if (wm == nullptr) return Shell::ExitFailure;
                if (args.size() == 1 || (args.size() == 2 && std::strcmp(args[1], "list") == 0))
                        return runList(ctx, *wm);
                const char *what = args[1];
                if (std::strcmp(what, "new") == 0) return runNew(ctx, *wm, args);
                if (args.size() == 2 && std::strcmp(what, "check") == 0) return runCheck(ctx, *wm);
                if (args.size() == 2 && std::strcmp(what, "stats") == 0) return runStats(ctx, *wm);
                if (args.size() == 3 && std::strcmp(what, "shuffle") == 0) {
                        long count = 0;
                        if (!parseNumber(args[2], 1'000'000, count)) return ctx.usage();
                        return runShuffle(ctx, *wm, count);
                }
                uint8_t id = 0;
                if (args.size() <= 3 && std::strcmp(what, "term") == 0) return runTerm(ctx, *wm, args);
                if (args.size() >= 4 && std::strcmp(what, "type") == 0) return runType(ctx, *wm, args);
                if (args.size() == 3 && std::strcmp(what, "dump") == 0) return runDump(ctx, *wm, args[2]);
                if (args.size() == 4 && std::strcmp(what, "scroll") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        char      *end = nullptr;
                        const long dy = std::strtol(args[3], &end, 0);
                        if (end == args[3] || *end != '\0' || dy < -4096 || dy > 4096) return ctx.usage();
                        TestWindow *t = testWindow(id);
                        if (t == nullptr) return report(ctx, Error::NotFound);
                        t->pending.fetch_add(int32_t(dy));
                        Error e = wm->update(id);
                        if (e.isOk()) e = wm->sync(SyncTimeoutMs);
                        if (e.isOk()) ctx.printf("scroll:   %lu us\n", static_cast<unsigned long>(t->scrollUs));
                        return report(ctx, e);
                }
                if (args.size() == 3 && std::strcmp(what, "pointer") == 0) {
                        if (std::strcmp(args[2], "on") != 0 && std::strcmp(args[2], "off") != 0) return ctx.usage();
                        wm->setPointerVisible(args[2][1] == 'n');
                        return report(ctx, wm->sync(SyncTimeoutMs));
                }
                if (args.size() >= 3 && std::strcmp(what, "key") == 0) return runKey(ctx, *wm, args);
                if (args.size() >= 4 && args.size() <= 6 && std::strcmp(what, "pointer") == 0) {
                        // As a mouse: motion, then the buttons (a mask) down after it, and the wheel.
                        long v[4] = {};
                        for (size_t i = 2; i < args.size(); ++i) {
                                char *end = nullptr;
                                v[i - 2] = std::strtol(args[i], &end, 0);
                                if (end == args[i] || *end != '\0' || v[i - 2] < -4096 || v[i - 2] > 4096)
                                        return ctx.usage();
                        }
                        PointerEvent e;
                        e.dx = static_cast<int16_t>(v[0]);
                        e.dy = static_cast<int16_t>(v[1]);
                        e.buttons = static_cast<uint8_t>(v[2]);
                        if (v[3] < -127 || v[3] > 127) return ctx.usage();
                        e.wheel = static_cast<int8_t>(v[3]);
                        wm->pointer(e);
                        const Error err = wm->sync(SyncTimeoutMs);
                        const Rect  at = wm->pointerArea();
                        if (err.isOk())
                                ctx.printf("pointer:  drawn in %ux%u at %u,%u\n", at.width, at.height, at.x, at.y);
                        return report(ctx, err);
                }
                if (args.size() == 3 && std::strcmp(what, "textbench") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        TestWindow *found = testWindow(id);
                        if (found == nullptr) return report(ctx, Error::NotFound);
                        TestWindow &t = *found;
                        t.textBench.store(true);
                        Error e = wm->update(id);
                        if (e.isOk()) e = wm->sync(SyncTimeoutMs);
                        if (e.isError()) return report(ctx, e);
                        ctx.printf("text:     %u rows of %u cells in %lu us\n", t.height / 16u,
                                   unsigned(t.width / 8u < MaxRowCells ? t.width / 8u : MaxRowCells),
                                   static_cast<unsigned long>(t.textUs));
                        // The pattern back.
                        e = wm->invalidate(id, Rect{0, 0, t.width, t.height});
                        if (e.isOk()) e = wm->sync(SyncTimeoutMs);
                        return report(ctx, e);
                }
                if (args.size() == 3 && std::strcmp(what, "close") == 0)
                        return parseId(ctx, args[2], id) ? report(ctx, wm->close(id)) : Shell::ExitUsage;
                if (args.size() == 3 && std::strcmp(what, "raise") == 0)
                        return parseId(ctx, args[2], id) ? report(ctx, wm->raise(id)) : Shell::ExitUsage;
                if (args.size() >= 3 && std::strcmp(what, "decorate") == 0) return runDecorate(ctx, *wm, args);
                if (args.size() >= 3 && std::strcmp(what, "title") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        char title[WindowManager::MaxTitle + 1] = {}; // the words joined by spaces
                        for (size_t i = 3, used = 0; i < args.size() && used < WindowManager::MaxTitle; ++i) {
                                const int n = std::snprintf(title + used, sizeof(title) - used, "%s%s",
                                                            i > 3 ? " " : "", args[i]);
                                if (n < 0) break;
                                used += size_t(n);
                        }
                        return report(ctx, wm->setTitle(id, StringView(title)));
                }
                if (args.size() == 3 && std::strcmp(what, "minimize") == 0)
                        return parseId(ctx, args[2], id) ? report(ctx, wm->minimize(id)) : Shell::ExitUsage;
                if (args.size() == 3 && std::strcmp(what, "maximize") == 0)
                        return parseId(ctx, args[2], id) ? report(ctx, wm->maximize(id)) : Shell::ExitUsage;
                if (args.size() == 4 && std::strcmp(what, "shade") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        if (std::strcmp(args[3], "on") != 0 && std::strcmp(args[3], "off") != 0) return ctx.usage();
                        Error e = wm->shade(id, args[3][1] == 'n');
                        if (e.isOk()) e = wm->sync(SyncTimeoutMs);
                        return report(ctx, e);
                }
                if (args.size() == 4 && std::strcmp(what, "above") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        if (std::strcmp(args[3], "on") != 0 && std::strcmp(args[3], "off") != 0) return ctx.usage();
                        return report(ctx, wm->setAbove(id, args[3][1] == 'n'));
                }
                if ((args.size() == 4 || args.size() == 5) && std::strcmp(what, "hid") == 0) {
                        // A raw key event, as a keyboard sends it: press, repeat or release of a HID usage
                        // with modifier bits (a test aid for keys `win key` can't type: Alt+Tab, arrows).
                        long usage = 0, modifiers = 0;
                        if (!parseNumber(args[3], 0xff, usage) ||
                            (args.size() == 5 && !parseNumber(args[4], 0xff, modifiers)))
                                return ctx.usage();
                        KeyEvent e;
                        e.usage = static_cast<uint8_t>(usage);
                        e.modifiers = static_cast<uint8_t>(modifiers);
                        if (std::strcmp(args[2], "press") == 0) {
                                e.action = KeyEvent::Action::Press;
                        } else if (std::strcmp(args[2], "release") == 0) {
                                e.action = KeyEvent::Action::Release;
                        } else if (std::strcmp(args[2], "repeat") == 0) {
                                e.action = KeyEvent::Action::Repeat;
                        } else {
                                return ctx.usage();
                        }
                        wm->key(e);
                        return report(ctx, wm->sync(SyncTimeoutMs));
                }
                if (args.size() == 3 && std::strcmp(what, "restore") == 0)
                        return parseId(ctx, args[2], id) ? report(ctx, wm->restore(id)) : Shell::ExitUsage;
                if (args.size() == 5 && std::strcmp(what, "resize") == 0) {
                        long w = 0, h = 0;
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        if (!parseNumber(args[3], UINT16_MAX, w) || !parseNumber(args[4], UINT16_MAX, h))
                                return ctx.usage();
                        return report(ctx, wm->resize(id, uint16_t(w), uint16_t(h)));
                }
                if (args.size() == 5 && (std::strcmp(what, "move") == 0 || std::strcmp(what, "drag") == 0)) {
                        long x = 0, y = 0;
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        if (!parseNumber(args[3], UINT16_MAX, x) || !parseNumber(args[4], UINT16_MAX, y))
                                return ctx.usage();
                        if (what[0] == 'd') return runDrag(ctx, *wm, id, x, y);
                        return report(ctx, wm->move(id, uint16_t(x), uint16_t(y)));
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(
        win, runWin,
        "[start|stop|list|new [X Y W H] [TITLE]|close N|raise N|move N X Y|drag N X Y|"
        "term [N]|type N TEXT|dump N|scroll N DY|textbench N|check|stats|shuffle N|bench [CELLS]|"
        "pointer DX DY [BUTTONS [WHEEL]]|decorate N [all|none|+NAME|-NAME...]|minimize N|maximize N|restore N|"
        "resize N W H|pointer on/off|key TEXT|title N [TEXT]|above N on/off|shade N on/off|hid press/release/repeat "
        "USAGE [MODS]]",
        "The desktop and its windows on display 0: 'new' opens a test window, 'drag' moves one "
        "as an outline (the palette is sysconf's wm/palette), 'check' compares every pixel with the stack, "
        "'pointer' moves the pointer as a mouse would (BUTTONS: 1 left, 2 right, 4 middle; WHEEL: notches, "
        "positive away), "
        "'key' types TEXT as a keyboard would (escapes as 'type'), "
        "'decorate' sets a window's frame (border, title, menu, minimize, maximize, close, resize)")

BMDS_NAMESPACE_END
