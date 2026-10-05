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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sdkconfig.h>
#include <esp_cache.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/font.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/utf8.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int SyncTimeoutMs = 1000;
        constexpr uint16_t     Square = 32; // the test pattern's squares
        constexpr int          DragSteps = 30;

        // A test window: a checker of two colors.
        struct TestWindow {
                        uint32_t a = 0;
                        uint32_t b = 0;

                        uint32_t at(uint32_t x, uint32_t y) const {
                                return ((x / Square + y / Square) & 1u) != 0 ? a : b;
                        }
        };

        constexpr TestWindow Palette[] = {
                {0xc04040, 0x803030}, {0x40c040, 0x308030}, {0x4060d0, 0x3040a0}, {0xc0c040, 0x909030},
                {0x40c0c0, 0x309090}, {0xc040c0, 0x903090}, {0xd08040, 0xa06030}, {0xa0a0a0, 0x707070},
        };

        // Window @p id's colors.
        const TestWindow &testWindow(uint8_t id) {
                return Palette[(id - 1u) % (sizeof(Palette) / sizeof(Palette[0]))];
        }

        // The squares in the painter's clip, a fill a color (32 rectangles a call).
        void redrawTest(void *arg, Painter &painter) {
                const TestWindow &t = *static_cast<const TestWindow *>(arg);
                const Rect        b = painter.clipBounds();
                Rect              as[32], bs[32];
                size_t            na = 0, nb = 0;
                for (uint32_t y = b.y / Square * Square; y < b.bottom(); y += Square) {
                        for (uint32_t x = b.x / Square * Square; x < b.right(); x += Square) {
                                const Rect r{uint16_t(x), uint16_t(y), Square, Square};
                                if (t.at(x, y) == t.a) {
                                        as[na++] = r;
                                } else {
                                        bs[nb++] = r;
                                }
                                if (na == 32) {
                                        (void)painter.fill(as, t.a);
                                        na = 0;
                                }
                                if (nb == 32) {
                                        (void)painter.fill(bs, t.b);
                                        nb = 0;
                                }
                        }
                }
                (void)painter.fill(Span<const Rect>(as, na), t.a);
                (void)painter.fill(Span<const Rect>(bs, nb), t.b);
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
                auto [wm, err] = WindowManager::manager(0);
                if (err.isOk()) err = wm->start(*d, color);
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
                        ctx.printf("%-4u %-5u %-5u %-5u %-5u %s%s\n", w.id, w.rect.x, w.rect.y, w.rect.width,
                                   w.rect.height, w.title, w.focused ? " (focus)" : "");
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
                config.arg = const_cast<TestWindow *>(&testWindow(guess));
                if (title[0] == '\0') std::snprintf(title, sizeof(title), "Window %u", guess);
                config.title = title;
                auto [id, err] = wm.open(config);
                if (err.isError()) return report(ctx, err);
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
        uint32_t frameAt(const WindowManager::Info &w, uint32_t x, uint32_t y) {
                const uint32_t frame = w.focused ? WindowManager::FocusedColor : WindowManager::FrameColor;
                const Rect     box = WindowFrame::closeBox(w.rect);
                if (inside(box, x, y)) {
                        const Rect mark{uint16_t(box.x + 4), uint16_t(box.y + 4), 4, 4};
                        return inside(mark, x, y) ? WindowManager::CloseMark : WindowManager::CloseColor;
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
                                return (bits & (0x80u >> (col % 8))) != 0 ? WindowManager::TitleColor : frame;
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
                const uint32_t desktop = wm.desktopColor();
                uint32_t       wrong = 0, checked = 0;
                int            fx = -1, fy = -1;
                uint32_t       fgot = 0, fwant = 0;
                for (uint32_t y = 0; y < s.height(); ++y) {
                        const uint8_t *line = buffer + size_t(y) * s.stride();
                        for (uint32_t x = 0; x < s.width(); ++x) {
                                uint32_t want = desktop;
                                for (size_t i = 0; i < n; ++i) {
                                        if (!inside(info[i].rect, x, y)) continue;
                                        const Rect &c = clients[i];
                                        want = inside(c, x, y) ? testWindow(info[i].id).at(x - c.x, y - c.y)
                                                               : frameAt(info[i], x, y);
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
                WindowManager *wm = running(ctx);
                if (wm == nullptr) return Shell::ExitFailure;
                if (args.size() == 1 || (args.size() == 2 && std::strcmp(args[1], "list") == 0))
                        return runList(ctx, *wm);
                const char *what = args[1];
                if (std::strcmp(what, "new") == 0) return runNew(ctx, *wm, args);
                if (args.size() == 2 && std::strcmp(what, "check") == 0) return runCheck(ctx, *wm);
                if (args.size() == 2 && std::strcmp(what, "stats") == 0) return runStats(ctx, *wm);
                if (args.size() == 3 && std::strcmp(what, "desktop") == 0) {
                        uint32_t color = 0;
                        if (!parseColor(args[2], color)) return ctx.usage();
                        return report(ctx, wm->setDesktopColor(color));
                }
                if (args.size() == 3 && std::strcmp(what, "shuffle") == 0) {
                        long count = 0;
                        if (!parseNumber(args[2], 1'000'000, count)) return ctx.usage();
                        return runShuffle(ctx, *wm, count);
                }
                uint8_t id = 0;
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

BMDS_SHELL_COMMAND(win, runWin,
                   "[start [RRGGBB]|stop|list|new [X Y W H] [TITLE]|close N|raise N|move N X Y|drag N X Y|"
                   "desktop RRGGBB|check|stats|shuffle N]",
                   "The desktop and its windows on display 0: 'new' opens a test window, 'drag' moves one "
                   "as an outline, 'check' compares every pixel with the stack")

BMDS_NAMESPACE_END
