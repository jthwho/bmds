/**
 * @file      displayppa.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `display ppa`: the PPA's checks for program surfaces (devplan/games.md §10b): whether the scaler
 * interpolates, the CLUT's expansion, the 565 doubling kernel; into a scratch picture in PSRAM,
 * not the screen. Their timings are benchmarks (benchdisplay.cpp). And `display rows`.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_cache.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/memory.h>
#include <bmds/ppa.h>
#include <bmds/random.h>
#include <bmds/shell.h>
#include <bmds/simd.h>
#include <bmds/staticobject.h>
#include <bmds/surface.h>

#include "ppatools.h"
#include <bmds/task.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using namespace PpaTools;

        // Scales a 1-pixel black-and-white checkerboard (RGB888, 64 × 64) by @p scale16 and counts
        // what came out: grays mean interpolation; with 32 (2×), mismatches against each pixel
        // doubled mean it isn't plain replication.
        void checkInterpolation(CommandContext &ctx, Ppa &ppa, Pictures &p, uint16_t scale16) {
                constexpr uint16_t N = 64;
                for (uint16_t y = 0; y < N; ++y)
                        for (uint16_t x = 0; x < N; ++x)
                                std::memset(p.src + (size_t(y) * N + x) * 3, ((x ^ y) & 1) ? 0xff : 0, 3);
                writeBack(p.src, size_t(N) * N * 3);
                std::memset(p.out, 0x55, p.outBytes);
                writeBack(p.out, p.outBytes);
                const Ppa::Picture from{p.src, N, N, Ppa::Format::Rgb888};
                const Ppa::Picture to{p.out, OutWidth, OutHeight, Ppa::Format::Rgb888};
                Error              err = ppa.scale(from, Rect{0, 0, N, N}, to, 0, 0, scale16, scale16, JobTimeoutMs);
                if (err.isOk()) err = ppa.wait(JobTimeoutMs);
                if (err.isError()) {
                        ctx.printf("interpolation at %u/16: %s\n", unsigned(scale16), err.desc());
                        return;
                }
                invalidate(p.out, p.outBytes);
                const uint16_t s = Ppa::scaled(N, scale16);
                uint32_t       black = 0, white = 0, gray = 0, untouched = 0, mismatch = 0;
                for (uint16_t y = 0; y < s; ++y) {
                        for (uint16_t x = 0; x < s; ++x) {
                                const uint8_t v = p.out[(size_t(y) * OutWidth + x) * 3];
                                if (v == 0) {
                                        ++black;
                                } else if (v == 0xff) {
                                        ++white;
                                } else if (v == 0x55) {
                                        ++untouched;
                                } else {
                                        ++gray;
                                }
                                if (scale16 == 32 && v != ((((x / 2) ^ (y / 2)) & 1) ? 0xff : 0)) ++mismatch;
                        }
                }
                ctx.printf("interpolation at %u/16 (%ux%u): %u black, %u white, %u gray, %u untouched",
                           unsigned(scale16), unsigned(s), unsigned(s), unsigned(black), unsigned(white),
                           unsigned(gray), unsigned(untouched));
                if (scale16 == 32) ctx.printf(", %u not replicated", unsigned(mismatch));
                ctx.printf("\n  first row:");
                for (uint16_t x = 0; x < 12 && x < s; ++x) ctx.printf(" %02x", p.out[size_t(x) * 3]);
                ctx.printf("\n");
                return;
        }

        // Expands random indices through a known palette and compares every pixel.
        void checkExpand(CommandContext &ctx, Ppa &ppa, Pictures &p, bool bypass) {
                uint32_t palette[Ppa::PaletteSize];
                for (uint32_t i = 0; i < Ppa::PaletteSize; ++i) palette[i] = (i << 16) | ((255 - i) << 8) | (i ^ 0x55);
                ppa.setPalette(palette, 0, Ppa::PaletteSize);
                fillRandom(p.src, 320 * 200);
                const Ppa::Picture from{p.src, 320, 200, Ppa::Format::L8};
                const Ppa::Picture to{p.mid, 320, 200, Ppa::Format::Rgb888};
                Error              err = ppa.expand(from, Rect{0, 0, 320, 200}, to, 0, 0, bypass, JobTimeoutMs);
                if (err.isOk()) err = ppa.wait(JobTimeoutMs);
                if (err.isError()) {
                        ctx.printf("expand check%s: %s\n", bypass ? " (bypass)" : "", err.desc());
                        return;
                }
                invalidate(p.mid, p.midBytes);
                uint32_t wrong = 0;
                for (size_t i = 0; i < 320 * 200; ++i) {
                        const uint32_t c = palette[p.src[i]];
                        const uint8_t *o = p.mid + i * 3;
                        if (o[0] != (c & 0xff) || o[1] != ((c >> 8) & 0xff) || o[2] != (c >> 16)) ++wrong;
                }
                ctx.printf("expand check%s: %u of 64000 pixels wrong\n", bypass ? " (bypass)" : "", unsigned(wrong));
                return;
        }


        // The PIE doubling kernel against the scalar one, on every row of a random 640x360 picture.
        void checkDoubling(CommandContext &ctx, Pictures &p) {
                constexpr uint16_t InW = 640, InH = 360;
                auto *rows = static_cast<uint16_t *>(Memory::allocAligned(MemSpace::Internal, size_t(InW) * 8, 64));
                if (rows == nullptr) {
                        ctx.printf("PIE doubling: %s\n", Error(Error::NoMem).desc());
                        return;
                }
                fillRandom(p.src, size_t(InW) * InH * 2);
                const auto *src = reinterpret_cast<const uint16_t *>(p.src);
                uint32_t    wrong = 0;
#if BMDS_HAS_PIE
                uint16_t *a = rows, *b = rows + size_t(InW) * 2;
                for (uint16_t y = 0; y < InH; ++y) {
                        double565Scalar(src + size_t(y) * InW, a, InW);
                        double565Pie(src + size_t(y) * InW, b, InW);
                        wrong += std::memcmp(a, b, size_t(InW) * 4) != 0 ? 1 : 0;
                }
#endif
                ctx.printf("PIE doubling against C: %u lines differ\n", unsigned(wrong));
                Memory::free(rows);
                return;
        }

        // The checks: whether the scaler interpolates, the CLUT's expansion, the doubling kernel.
        void runChecks(CommandContext &ctx, Ppa &ppa, Pictures &p) {
                checkInterpolation(ctx, ppa, p, 32);
                checkInterpolation(ctx, ppa, p, 24);
                checkExpand(ctx, ppa, p, false);
                checkDoubling(ctx, p);
                return;
        }

        // `display rows`: a test surface scanned with its rows repeated.
        constinit StaticObject<Surface> rowsSurface;

        // Bands of color 8 rows tall with a white row on top of each, a gray column every 64 pixels.
        Error drawRows(Surface &surface) {
                static constexpr uint32_t Colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff};
                Error                     err;
                for (uint16_t y = 0; y < surface.height() && err.isOk(); y += 8) {
                        const uint16_t h = uint16_t(surface.height() - y < 8 ? surface.height() - y : 8);
                        err = surface.fill(Rect{0, y, surface.width(), h}, Colors[(y / 8) % 6], WaitForever);
                        if (err.isOk()) err = surface.fill(Rect{0, y, surface.width(), 1}, 0xffffff, WaitForever);
                }
                for (uint16_t x = 0; x < surface.width() && err.isOk(); x += 64)
                        err = surface.fill(Rect{x, 0, 4, surface.height()}, 0x808080, WaitForever);
                return err;
        }

        // What drawRows() (or, @p stripes, drawStripes()) put at row @p y, x 10 (off the columns).
        uint32_t expectedRow(uint16_t y, bool stripes) {
                static constexpr uint32_t Colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff};
                if (stripes) return (y & 1) ? 0x000000 : 0xffffff;
                return y % 8 == 0 ? 0xffffff : Colors[(y / 8) % 6];
        }

        // Rows white and black in turn, one surface row each: on the screen, every stripe the same
        // height (the factor) when every line reads the right row.
        Error drawStripes(Surface &surface) {
                Error  err = surface.fill(surface.bounds(), 0x000000, WaitForever);
                Rect   rows[32];
                size_t n = 0;
                for (uint16_t y = 0; y < surface.height() && err.isOk(); y += 2) {
                        rows[n++] = Rect{0, y, surface.width(), 1};
                        if (n == 32 || y + 2 >= surface.height()) {
                                err = surface.fill(Span<const Rect>(rows, n), 0xffffff, WaitForever);
                                n = 0;
                        }
                }
                return err;
        }

        // Reads the surface back and checks every row at three columns against what was drawn.
        void verifyRows(CommandContext &ctx, const Surface &surface, bool stripes) {
                const uint8_t *buffer = surface.shown();
                invalidate(buffer, surface.stride() * surface.height());
                uint32_t bad = 0;
                for (uint16_t y = 0; y < surface.height(); ++y) {
                        const uint32_t want = expectedRow(y, stripes);
                        for (uint16_t x : {uint16_t(10), uint16_t(600), uint16_t(1270)}) {
                                const uint8_t *p = buffer + y * surface.stride() + x * 3;
                                const uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                if (got == want) continue;
                                if (bad < 8)
                                        ctx.printf("  row %u x %u: %06x, wanted %06x\n", unsigned(y), unsigned(x),
                                                   unsigned(got), unsigned(want));
                                ++bad;
                        }
                }
                ctx.printf("surface check: %u of %u pixels wrong\n", unsigned(bad), unsigned(surface.height()) * 3);
                return;
        }

} // namespace

namespace detail {

        // display ppa: the PPA's checks (its timings are benchmarks: `bench run display.ppa`).
        int runDisplayPpa(CommandContext &ctx) {
                if (ctx.args().size() != 2) return ctx.usage();
                Pictures pictures;
                Error    err = pictures.allocate();
                Ppa      ppa;
                if (err.isOk()) err = ppa.start();
                if (err.isError()) {
                        ctx.eprintf("display: ppa: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                runChecks(ctx, ppa, pictures);
                return Shell::ExitSuccess;
        }

        // display rows N: surface 0's mode scanned from a surface of 720 / N rows, each N times (1: back).
        int runDisplayRows(CommandContext &ctx) {
                const auto args = ctx.args();
                // display rows N [stripes] [irq K]: K, one line descriptor in K interrupts (0: the
                // default); stripes, rows white and black in turn instead of colored bands.
                if (args.size() < 3) return ctx.usage();
                bool     stripes = false;
                uint32_t irq = 0;
                for (size_t i = 3; i < args.size(); ++i) {
                        if (std::strcmp(args[i], "stripes") == 0) {
                                stripes = true;
                        } else if (std::strcmp(args[i], "irq") == 0 && i + 1 < args.size()) {
                                irq = static_cast<uint32_t>(std::atoi(args[++i]));
                        } else {
                                return ctx.usage();
                        }
                }
                const int n = std::atoi(args[2]);
                auto [d, err] = Display::display(0);
                if (err.isError() || !d->isRunning() || n < 1 || n > 16) {
                        ctx.eprintf("display: rows: %s\n", err.isError() ? err.desc() : "not running, or N not 1-16");
                        return Shell::ExitFailure;
                }
                Scanout &scan = d->scanout();
                scan.setLineInterrupts(irq);
                if (!rowsSurface.isCreated()) rowsSurface.create();
                Surface &test = *rowsSurface;
                uint64_t start = System::uptimeUs();
                if (n == 1) {
                        err = scan.show(d->surface(), 1000);
                        if (err.isOk()) test.stop();
                } else {
                        if (scan.surface() == &test) err = scan.show(d->surface(), 1000);
                        test.stop();
                        if (err.isOk())
                                err = test.start(d->surface().width(), uint16_t(d->surface().height() / n),
                                                 Surface::Buffering::Single);
                        if (err.isOk()) err = stripes ? drawStripes(test) : drawRows(test);
                        if (err.isOk()) verifyRows(ctx, test, stripes);
                        start = System::uptimeUs();
                        if (err.isOk()) err = scan.show(test, 1000);
                }
                const uint32_t switchUs = static_cast<uint32_t>(System::uptimeUs() - start);
                if (err.isError()) {
                        ctx.eprintf("display: rows: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const Scanout::Stats before = scan.stats();
                Task::sleep(2000);
                const Scanout::Stats after = scan.stats();
                ctx.printf(
                        "rows %d: switched in %u us; 2 s: %u frames, %u descriptors a frame, %u underruns, %u stalls, "
                        "%u resyncs, %u rebuilds, %u fallbacks, slowest interrupt %u cycles, %u cycles a frame\n",
                        n, unsigned(switchUs), unsigned(after.frames - before.frames), unsigned(after.blocksPerFrame),
                        unsigned(after.underruns - before.underruns), unsigned(after.stalls - before.stalls),
                        unsigned(after.resyncs - before.resyncs), unsigned(after.rebuilds - before.rebuilds),
                        unsigned(after.fallbacks - before.fallbacks), unsigned(after.isrCyclesMax),
                        after.frames != before.frames
                                ? unsigned((after.isrCycles - before.isrCycles) / (after.frames - before.frames))
                                : 0u);
                return Shell::ExitSuccess;
        }

} // namespace detail

BMDS_NAMESPACE_END
