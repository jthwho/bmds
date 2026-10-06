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
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/terminal.h>
#include <bmds/terminalview.h>
#include <bmds/utf8.h>
#include <bmds/windowmanager.h>
#include <bmds/windowpalette.h>

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

        constinit TestWindow testWindows[WindowStack::MaxWindows]; // by number - 1

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

        bool parseColor(const char *text, uint32_t &out) {
                char *end = nullptr;
                out = static_cast<uint32_t>(std::strtoul(text, &end, 16));
                return end != text && *end == '\0' && out <= 0xffffff;
        }

        int report(CommandContext &ctx, Error err) {
                if (err.isOk()) return Shell::ExitSuccess;
                ctx.eprintf("win: %s\n", err.desc());
                return Shell::ExitFailure;
        }

        // win start [RRGGBB]
        int runStart(CommandContext &ctx, Args args) {
                uint32_t color = CONFIG_BMDS_DESKTOP_COLOR;
                if (args.size() == 3 && !parseColor(args[2], color)) return ctx.usage();
                auto [d, derr] = Display::display(0);
                if (derr.isError() || !d->isRunning()) {
                        ctx.eprintf("win: the display isn't running ('display start')\n");
                        return Shell::ExitFailure;
                }
                WindowPalette palette;
                palette.setColor(WindowPalette::Role::Desktop, color);
                auto [wm, err] = WindowManager::manager(0);
                if (err.isOk()) err = wm->start(*d, palette);
                if (err == Error::Busy) {
                        ctx.eprintf("win: display in use by the %s\n", d->owner());
                        return Shell::ExitFailure;
                }
                return report(ctx, err);
        }

        int runList(CommandContext &ctx, WindowManager &wm) {
                WindowManager::Info info[WindowStack::MaxWindows];
                const size_t        n = wm.list(info);
                ctx.printf("Win  X     Y     W     H     Title\n");
                for (size_t i = 0; i < n; ++i) {
                        const WindowManager::Info &w = info[i];
                        const Terminal            *term = Terminal::byWindow(w.id);
                        ctx.printf("%-4u %-5u %-5u %-5u %-5u %s%s%s\n", w.id, w.rect.x, w.rect.y, w.rect.width,
                                   w.rect.height, w.title, w.focused ? " (focus)" : "",
                                   term != nullptr ? " (terminal)" : "");
                }
                return Shell::ExitSuccess;
        }

        // win new [X Y W H] [TITLE...]
        int runNew(CommandContext &ctx, WindowManager &wm, Args args) {
                WindowManager::Info         info[WindowStack::MaxWindows];
                const size_t                open = wm.list(info);
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
                // Numbers are the lowest free (WindowStack): the new window's is
                // known beforehand, as only this command opens windows for now.
                uint8_t guess = 1;
                for (bool used = true; used && guess <= WindowStack::MaxWindows;) {
                        used = false;
                        for (size_t i = 0; i < open; ++i) used |= info[i].id == guess;
                        if (used) ++guess;
                }
                if (guess > WindowStack::MaxWindows) return report(ctx, Error::Full);
                TestWindow &t = testWindows[guess - 1];
                t.id = guess;
                t.a = Palette[(guess - 1u) % (sizeof(Palette) / sizeof(Palette[0]))][0];
                t.b = Palette[(guess - 1u) % (sizeof(Palette) / sizeof(Palette[0]))][1];
                t.offset = 0;
                t.pending.store(0);
                const Rect client = WindowFrame::client(config.rect);
                t.width = client.width; // until the window is placed (below)
                t.height = client.height;
                config.arg = &t;
                if (title[0] == '\0') std::snprintf(title, sizeof(title), "Window %u", guess);
                config.title = title;
                auto [id, err] = wm.open(config);
                if (err.isError()) return report(ctx, err);
                auto [placed, perr] = wm.clientRect(id);
                if (perr.isOk()) {
                        t.width = placed.width;
                        t.height = placed.height;
                }
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
                WindowManager::Info info[WindowStack::MaxWindows];
                const size_t        n = wm.list(info);
                const Rect         *from = nullptr;
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

        // What window @p w's frame shows at (@p x, @p y).
        uint32_t frameAt(const WindowPalette &palette, const WindowManager::Info &w, uint32_t x, uint32_t y) {
                using Role = WindowPalette::Role;
                const uint32_t frame = palette.color(w.focused ? Role::ActiveFrame : Role::InactiveFrame);
                const uint32_t text = palette.color(w.focused ? Role::ActiveTitle : Role::InactiveTitle);
                const Rect     box = WindowFrame::closeBox(w.rect);
                if (inside(box, x, y)) {
                        const Rect mark{uint16_t(box.x + 4), uint16_t(box.y + 4), 4, 4};
                        return palette.color(inside(mark, x, y) ? Role::CloseMark : Role::CloseBox);
                }
                const Rect t = WindowFrame::title(w.rect);
                if (!inside(t, x, y)) return frame;
                const StringView title(w.title);
                const Font      &font = Fonts::unscii16;
                uint32_t         left = t.x;
                for (size_t i = 0; i < title.size();) {
                        const Glyph g = font.glyph(Utf8::decode(title, i));
                        if (left + g.width - t.x > t.width) break;
                        if (x < left + g.width) {
                                const uint32_t col = x - left;
                                const uint8_t  bits = g.rows[(y - t.y) * (g.width / 8u) + col / 8];
                                return (bits & (0x80u >> (col % 8))) != 0 ? text : frame;
                        }
                        left += g.width;
                }
                return frame;
        }

        // Every pixel of the shown picture against the stack: the desktop's
        // color, a frame (its title's glyphs too), or a test window's pattern.
        int runCheck(CommandContext &ctx, WindowManager &wm) {
                Error err = wm.sync(SyncTimeoutMs);
                if (err.isError()) return report(ctx, err);
                auto [d, derr] = Display::display(0);
                if (derr.isError()) return report(ctx, derr);
                const Surface      &s = d->surface();
                WindowManager::Info info[WindowStack::MaxWindows];
                const size_t        n = wm.list(info);
                Rect                clients[WindowStack::MaxWindows];
                for (size_t i = 0; i < n; ++i) clients[i] = WindowFrame::client(info[i].rect);
                const uint8_t *buffer = s.shown();
                const size_t   bytes = s.stride() * s.height();
                for (size_t off = 0; off < bytes; off += 64 * 1024)
                        (void)esp_cache_msync(const_cast<uint8_t *>(buffer) + off,
                                              bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                // Each window's label, drawn by the scalar kernel (a reference for the painter's).
                constexpr uint16_t LabelPixels = MaxLabel * 16;
                const size_t       labelBytes = size_t(LabelPixels) * 3 * Fonts::unscii16.height();
                auto *labels = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, labelBytes * (n > 0 ? n : 1)));
                if (labels == nullptr) return report(ctx, Error::NoMem);
                Rect          labelRects[WindowStack::MaxWindows];
                GlyphRenderer reference(GlyphRenderer::Kernel::Scalar);
                for (size_t i = 0; i < n; ++i) {
                        const TestWindow &t = testWindows[info[i].id - 1];
                        TextCell          cells[MaxLabel];
                        const size_t      count = makeLabel(t.id, cells);
                        size_t            used = 0;
                        const uint16_t    w = reference.drawCells(Span<const TextCell>(cells, count), Fonts::unscii16,
                                                                  labels + i * labelBytes, size_t(LabelPixels) * 3,
                                                                  labelWidth(t), used);
                        labelRects[i] = Rect{uint16_t(clients[i].x + LabelX), uint16_t(clients[i].y + LabelY), w,
                                             Fonts::unscii16.height()};
                }
                const WindowPalette palette = wm.palette();
                // Each terminal's client area, drawn from its screen by the scalar kernel.
                uint8_t *images[WindowStack::MaxWindows] = {};
                for (size_t i = 0; i < n; ++i) {
                        Terminal *term = Terminal::byWindow(info[i].id);
                        if (term == nullptr) continue;
                        const Rect  &c = clients[i];
                        const size_t stride = size_t(c.width) * 3;
                        images[i] = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, stride * c.height));
                        if (images[i] == nullptr) {
                                for (uint8_t *image : images) Memory::free(image);
                                Memory::free(labels);
                                return report(ctx, Error::NoMem);
                        }
                        const uint32_t back = palette.color(WindowPalette::Role::TerminalBackground);
                        for (size_t px = 0; px < size_t(c.width) * c.height; ++px) {
                                images[i][3 * px] = static_cast<uint8_t>(back);
                                images[i][3 * px + 1] = static_cast<uint8_t>(back >> 8);
                                images[i][3 * px + 2] = static_cast<uint8_t>(back >> 16);
                        }
                        ScreenConsole::Locked locked(term->console());
                        const TerminalScreen &screen = locked.screen();
                        TextCell              row[Terminal::MaxColumns];
                        for (uint16_t r = 0; r < screen.rows(); ++r) {
                                const int    cursor = screen.isCursorVisible() && r == screen.cursorRow()
                                                              ? screen.cursorColumn()
                                                              : -1;
                                const size_t cells =
                                        TerminalView::row(screen, r, palette, cursor, row, Terminal::MaxColumns);
                                size_t used = 0;
                                (void)reference.drawCells(Span<const TextCell>(row, cells), Fonts::unscii16,
                                                          images[i] + size_t(r) * 16 * stride, stride,
                                                          uint16_t(screen.columns() * 8), used);
                        }
                }
                const uint32_t desktop = palette.color(WindowPalette::Role::Desktop);
                const Rect     pointer = wm.pointerArea(); // over everything: not modelled
                uint32_t       wrong = 0, checked = 0;
                int            fx = -1, fy = -1;
                uint32_t       fgot = 0, fwant = 0;
                for (uint32_t y = 0; y < s.height(); ++y) {
                        const uint8_t *line = buffer + size_t(y) * s.stride();
                        for (uint32_t x = 0; x < s.width(); ++x) {
                                if (inside(pointer, x, y)) continue;
                                uint32_t want = desktop;
                                for (size_t i = 0; i < n; ++i) {
                                        if (!inside(info[i].rect, x, y)) continue;
                                        const Rect &c = clients[i];
                                        if (!inside(c, x, y)) {
                                                want = frameAt(palette, info[i], x, y);
                                        } else if (images[i] != nullptr) {
                                                const uint8_t *q =
                                                        images[i] + (size_t(y - c.y) * c.width + (x - c.x)) * 3;
                                                want = uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
                                        } else if (inside(labelRects[i], x, y)) {
                                                const uint8_t *q = labels + i * labelBytes +
                                                                   (y - labelRects[i].y) * size_t(LabelPixels) * 3 +
                                                                   (x - labelRects[i].x) * 3;
                                                want = uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
                                        } else {
                                                want = testWindows[info[i].id - 1].at(x - c.x, y - c.y);
                                        }
                                        break;
                                }
                                ++checked;
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
                           static_cast<unsigned long>(checked));
                if (wrong > 0)
                        ctx.printf(" (the first at %d,%d: %06lx, not %06lx)", fx, fy, static_cast<unsigned long>(fgot),
                                   static_cast<unsigned long>(fwant));
                ctx.printf("\n");
                for (uint8_t *image : images) Memory::free(image);
                Memory::free(labels);
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
                        WindowManager::Info info[WindowStack::MaxWindows];
                        const size_t        n = wm.list(info);
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

        // win color [ROLE RRGGBB]
        int runColor(CommandContext &ctx, WindowManager &wm, Args args) {
                WindowPalette palette = wm.palette();
                if (args.size() == 2) {
                        for (size_t i = 0; i < WindowPalette::RoleCount; ++i) {
                                const auto role = static_cast<WindowPalette::Role>(i);
                                ctx.printf("%-15s %06lx\n", WindowPalette::name(role),
                                           static_cast<unsigned long>(palette.color(role)));
                        }
                        return Shell::ExitSuccess;
                }
                auto [role, err] = WindowPalette::find(args[2]);
                if (err.isError()) {
                        ctx.eprintf("win: %s: not a color role ('win color' lists them)\n", args[2]);
                        return Shell::ExitUsage;
                }
                uint32_t color = 0;
                if (!parseColor(args[3], color)) return ctx.usage();
                palette.setColor(role, color);
                return report(ctx, wm.setPalette(palette));
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

        // win term [N]: terminal N (or the first not shown) in a window.
        int runTerm(CommandContext &ctx, WindowManager &wm, Args args) {
                size_t number = Terminal::MaxTerminals;
                if (args.size() == 3) {
                        long v = 0;
                        if (!parseNumber(args[2], long(Terminal::MaxTerminals) - 1, v)) return ctx.usage();
                        number = size_t(v);
                } else {
                        for (size_t i = 0; i < Terminal::MaxTerminals && number == Terminal::MaxTerminals; ++i) {
                                auto [t, err] = Terminal::terminal(i);
                                if (err.isOk() && t->window() == 0) number = i;
                        }
                        if (number == Terminal::MaxTerminals) return report(ctx, Error::Full);
                }
                auto [term, err] = Terminal::terminal(number);
                if (err.isError()) return report(ctx, err);
                // 100 × 30 cells, cascading.
                const uint16_t at = static_cast<uint16_t>(32 + 32 * number);
                auto [id, oerr] = term->open(wm, Rect{at, at, 100 * 8 + 2 * WindowFrame::Border,
                                                      30 * 16 + WindowFrame::TitleHeight + WindowFrame::Border});
                if (oerr.isError()) return report(ctx, oerr);
                ctx.printf("win %u: terminal %u, %ux%u\n", id, unsigned(number), term->console().size().columns,
                           term->console().size().rows);
                return Shell::ExitSuccess;
        }

        // The terminal in window @p text, or null with a message.
        Terminal *terminalIn(CommandContext &ctx, const char *text) {
                uint8_t id = 0;
                if (!parseId(ctx, text, id)) return nullptr;
                Terminal *term = Terminal::byWindow(id);
                if (term == nullptr) ctx.eprintf("win: %u: not a terminal\n", id);
                return term;
        }

        // win type N TEXT...: typed into window N's terminal; C escapes and ^X controls.
        int runType(CommandContext &ctx, Args args) {
                Terminal *term = terminalIn(ctx, args[2]);
                if (term == nullptr) return Shell::ExitFailure;
                uint8_t buf[256];
                size_t  n = 0;
                for (size_t a = 3; a < args.size(); ++a) {
                        if (a > 3 && n < sizeof(buf)) buf[n++] = ' ';
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
                const size_t taken = term->console().pushInput(ByteSpan(buf, n));
                if (taken < n) ctx.eprintf("win: input full: %u of %u bytes typed\n", unsigned(taken), unsigned(n));
                return taken == n ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        // win dump N: window N's terminal screen as text, the cursor's place.
        int runDump(CommandContext &ctx, const char *text) {
                Terminal *term = terminalIn(ctx, text);
                if (term == nullptr) return Shell::ExitFailure;
                ScreenConsole::Locked locked(term->console());
                const TerminalScreen &screen = locked.screen();
                for (uint16_t r = 0; r < screen.rows(); ++r) {
                        char   line[Terminal::MaxColumns * 4 + 1];
                        size_t n = 0, end = 0;
                        for (uint16_t c = 0; c < screen.columns(); ++c) {
                                const TerminalCell &cell = screen.row(r)[c];
                                if (cell.tail != 0) continue;
                                n += Utf8::encode(cell.codepoint, line + n);
                                if (cell.codepoint != ' ') end = n;
                        }
                        line[end] = '\0';
                        ctx.printf("%2u|%s\n", r, line);
                }
                ctx.printf("cursor:   row %u, column %u%s\n", screen.cursorRow(), screen.cursorColumn(),
                           screen.isCursorVisible() ? "" : " (hidden)");
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
                return Shell::ExitSuccess;
        }

        int runWin(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() >= 2 && std::strcmp(args[1], "start") == 0 && args.size() <= 3)
                        return runStart(ctx, args);
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
                if (std::strcmp(what, "color") == 0 && (args.size() == 2 || args.size() == 4))
                        return runColor(ctx, *wm, args);
                if (args.size() == 3 && std::strcmp(what, "shuffle") == 0) {
                        long count = 0;
                        if (!parseNumber(args[2], 1'000'000, count)) return ctx.usage();
                        return runShuffle(ctx, *wm, count);
                }
                uint8_t id = 0;
                if (args.size() <= 3 && std::strcmp(what, "term") == 0) return runTerm(ctx, *wm, args);
                if (args.size() >= 4 && std::strcmp(what, "type") == 0) return runType(ctx, args);
                if (args.size() == 3 && std::strcmp(what, "dump") == 0) return runDump(ctx, args[2]);
                if (args.size() == 4 && std::strcmp(what, "scroll") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        char      *end = nullptr;
                        const long dy = std::strtol(args[3], &end, 0);
                        if (end == args[3] || *end != '\0' || dy < -4096 || dy > 4096) return ctx.usage();
                        testWindows[id - 1].pending.fetch_add(int32_t(dy));
                        Error e = wm->update(id);
                        if (e.isOk()) e = wm->sync(SyncTimeoutMs);
                        if (e.isOk())
                                ctx.printf("scroll:   %lu us\n",
                                           static_cast<unsigned long>(testWindows[id - 1].scrollUs));
                        return report(ctx, e);
                }
                if ((args.size() == 4 || args.size() == 5) && std::strcmp(what, "pointer") == 0) {
                        // As a mouse: motion, then the buttons (a mask) down after it.
                        long v[3] = {};
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
                        wm->pointer(e);
                        const Error err = wm->sync(SyncTimeoutMs);
                        const Rect  at = wm->pointerArea();
                        if (err.isOk())
                                ctx.printf("pointer:  drawn in %ux%u at %u,%u\n", at.width, at.height, at.x, at.y);
                        return report(ctx, err);
                }
                if (args.size() == 3 && std::strcmp(what, "textbench") == 0) {
                        if (!parseId(ctx, args[2], id)) return Shell::ExitUsage;
                        TestWindow &t = testWindows[id - 1];
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
        "[start [RRGGBB]|stop|list|new [X Y W H] [TITLE]|close N|raise N|move N X Y|drag N X Y|"
        "term [N]|type N TEXT|dump N|scroll N DY|textbench N|color [ROLE RRGGBB]|check|stats|shuffle N|bench [CELLS]|"
        "pointer DX DY [BUTTONS]]",
        "The desktop and its windows on display 0: 'new' opens a test window, 'drag' moves one "
        "as an outline, 'color' lists or sets the palette, 'check' compares every pixel with the stack, "
        "'pointer' moves the pointer as a mouse would (BUTTONS: 1 left, 2 right, 4 middle)")

BMDS_NAMESPACE_END
