/**
 * @file      benchdisplay.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The display's benchmarks (devplan/bench.md §3): the PPA at the games' sizes, the pixel path of an
 * integer scale, a surface's blits (were `display ppa`, `ppa soak`, `ppa path`, `ppa blitcost`:
 * into scratch pictures, not the screen), and drawing on surface 0 (were `display test`, `fill`,
 * `present`, `anim`: only while nothing else has the display, before the desktop starts).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_cache.h>
#include <hal/cache_ll.h>
#include <bmds/bench.h>
#include <bmds/blitchain.h>
#include <bmds/display.h>
#include <bmds/memory.h>
#include <bmds/image.h>
#include <bmds/ppa.h>
#include <bmds/stopwatch.h>
#include <bmds/surface.h>
#include <bmds/task.h>

#include "ppatools.h"

BMDS_NAMESPACE_BEGIN

namespace detail {
        Error drawTestPicture(Surface &surface); // displaycommand.cpp
} // namespace detail

namespace {

        using namespace PpaTools;

        constexpr unsigned int TimeoutMs = 100;

        // The PPA's jobs were timed 20 times a case.
        constexpr BenchParam PpaParams[] = {
                {"runs", "20", "Jobs timed a case"},
        };

        constexpr BenchParam SoakParams[] = {
                {"seconds", "5", "How long a run scales, flat out"},
                {"runs", "1", "Runs"},
                {"warmup", "0", "Runs first, not kept"},
        };

        constexpr BenchParam PresentParams[] = {
                {"runs", "20", "Presents timed"},
        };

        constexpr BenchParam AnimParams[] = {
                {"squares", "8", "Squares bouncing (1-64)"},
                {"seconds", "5", "How long it runs"},
                {"runs", "1", "Runs"},
                {"warmup", "0", "Runs first, not kept"},
        };

        // Times @p job (a PPA job started, or a chain submitted) and @p wait for it, in µs.
        template <typename Job, typename Wait>
        Error timeJob(BenchContext &ctx, const char *metric, Job job, Wait wait) {
                return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        Error     err = job();
                        if (err.isOk()) err = wait();
                        out = double(watch.elapsedUs());
                        return err;
                });
        }

        // ---- The PPA (devplan/games.md §10b) ----

        Error runPpaScale(BenchContext &ctx) {
                Pictures p;
                Ppa      ppa;
                Error    err = p.allocate();
                if (err.isOk()) err = ppa.start();
                if (err.isError()) return err;
                const Ppa::Picture out{p.out, OutWidth, OutHeight, Ppa::Format::Rgb888};
                fillRandom(p.src, p.srcBytes);
                const auto wait = [&] {
                        return ppa.wait(JobTimeoutMs);
                };
                struct Case {
                                const char *metric;
                                uint16_t    w, h;
                                Ppa::Format format;
                                uint16_t    x, y, sx, sy; // where, and the scales in sixteenths
                };
                const Case cases[] = {
                        {"blocks_565_640x360_x2", 640, 360, Ppa::Format::Rgb565, 0, 0, 32, 32},
                        {"doom_888_320x200_x3", 320, 200, Ppa::Format::Rgb888, 160, 4, 48, 57},
                        {"integer_888_320x200_x3", 320, 200, Ppa::Format::Rgb888, 160, 60, 48, 48},
                        {"solitaire_565_640x480_x1", 640, 480, Ppa::Format::Rgb565, 0, 0, 16, 16},
                        {"copy_888_1280x720", 1280, 720, Ppa::Format::Rgb888, 0, 0, 16, 16},
                };
                for (const Case &c : cases) {
                        const Ppa::Picture from{p.src, c.w, c.h, c.format};
                        err = timeJob(
                                ctx, c.metric,
                                [&] {
                                        return ppa.scale(from, Rect{0, 0, c.w, c.h}, out, c.x, c.y, c.sx, c.sy,
                                                         JobTimeoutMs);
                                },
                                wait);
                        if (err.isError()) return err;
                }
                const Ppa::Picture l8{p.src, 320, 200, Ppa::Format::L8};
                const Ppa::Picture mid{p.mid, 320, 200, Ppa::Format::Rgb888};
                err = timeJob(
                        ctx, "expand_l8_320x200",
                        [&] { return ppa.expand(l8, Rect{0, 0, 320, 200}, mid, 0, 0, false, JobTimeoutMs); }, wait);
                if (err.isError()) return err;
                return timeJob(
                        ctx, "doom_expand_and_scale",
                        [&] {
                                Error e = ppa.expand(l8, Rect{0, 0, 320, 200}, mid, 0, 0, false, JobTimeoutMs);
                                if (e.isOk()) e = ppa.wait(JobTimeoutMs);
                                return e.isOk()
                                               ? ppa.scale(mid, Rect{0, 0, 320, 200}, out, 160, 4, 48, 57, JobTimeoutMs)
                                               : e;
                        },
                        wait);
        }

        // 640x360 RGB565 scaled by 2 into a 1280x720 picture, flat out: the PSRAM's write rate.
        Error runPpaSoak(BenchContext &ctx) {
                uint64_t seconds = 0;
                if (ctx.paramUnsigned("seconds", seconds).isError() || seconds == 0 || seconds > 600)
                        return Error::Invalid;
                Pictures p;
                Ppa      ppa;
                Error    err = p.allocate();
                if (err.isOk()) err = ppa.start();
                if (err.isError()) return err;
                fillRandom(p.src, p.srcBytes);
                const Ppa::Picture from{p.src, 640, 360, Ppa::Format::Rgb565};
                const Ppa::Picture out{p.out, OutWidth, OutHeight, Ppa::Format::Rgb888};
                double             mbs = 0;
                err = ctx.measure("frames_a_second", "/s", BenchBetter::Higher, [&](double &fps) {
                        Stopwatch watch;
                        uint32_t  jobs = 0;
                        Error     e;
                        while (e.isOk() && watch.elapsedUs() < int64_t(seconds) * 1'000'000 && !ctx.isCancelled()) {
                                e = ppa.scale(from, Rect{0, 0, 640, 360}, out, 0, 0, 32, 32, JobTimeoutMs);
                                if (e.isOk()) e = ppa.wait(JobTimeoutMs);
                                ++jobs;
                        }
                        const double us = double(watch.elapsedUs());
                        fps = jobs * 1e6 / us;
                        mbs = jobs * double(OutWidth) * OutHeight * 3 / us;
                        return e;
                });
                if (err.isOk()) ctx.report("written_mb_s", "MB/s", BenchBetter::Higher, mbs);
                return err;
        }

        // The pixel path for an integer scale (Blocks: 640x360 RGB565 at 2x): the CPU doubles each line
        // into a strip in internal RAM, the 2D-DMA widens each strip row into rows of RGB888 (devplan/
        // games.md §10b''').
        Error runPixelPath(BenchContext &ctx) {
                constexpr uint16_t InW = 640, InH = 360, Rows = 12;         // 360 = 30 strips
                constexpr size_t   StripBytes = size_t(InW) * 2 * 2 * Rows; // doubled width, RGB565
                constexpr size_t   FetchBytes = size_t(InW) * 2 * Rows;
                Pictures           p;
                if (Error err = p.allocate(); err.isError()) return err;
                auto *strips = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 2 * StripBytes, 64));
                auto *fetched = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 2 * FetchBytes, 64));
                BlitChain chain, fetch;
                Error     err = strips == nullptr || fetched == nullptr ? Error(Error::NoMem) : Error(Error::Ok);
                if (err.isOk()) err = chain.start(BlitChain::Conversion::Rgb565ToRgb888, 2 * Rows);
                if (err.isOk()) err = fetch.start(BlitChain::Conversion::None, 1);
                const auto done = [&] {
                        chain.stop();
                        fetch.stop();
                        Memory::free(strips);
                        Memory::free(fetched);
                        return err;
                };
                if (err.isError()) return done();
                fillRandom(p.src, size_t(InW) * InH * 2);
                const auto *src = reinterpret_cast<const uint16_t *>(p.src);
                auto        cpu = [&](bool pie, uint8_t *strip, uint16_t y0) {
                        for (uint16_t r = 0; r < Rows; ++r) {
                                auto *o = reinterpret_cast<uint16_t *>(strip + size_t(r) * InW * 4);
#if BMDS_HAS_PIE
                                if (pie) {
                                        double565Pie(src + size_t(y0 + r) * InW, o, InW);
                                        continue;
                                }
#endif
                                double565Scalar(src + size_t(y0 + r) * InW, o, InW);
                        }
                        writeBack(strip, StripBytes);
                        return;
                };
                const auto cpuTime = [&](const char *metric, bool pie, bool cached) {
                        return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                                Stopwatch watch;
                                for (uint16_t y = 0; y < InH; y += Rows) cpu(pie, strips, cached ? 0 : y);
                                out = double(watch.elapsedUs());
                                return Error(Error::Ok);
                        });
                };
                err = cpuTime("cpu_double_scalar", false, false);
                if (err.isOk() && BMDS_HAS_PIE) err = cpuTime("cpu_double_pie", true, false);
                // The same strip's source lines each time: in the cache, the doubling and the write-back alone.
                if (err.isOk()) err = cpuTime("cpu_double_cached", BMDS_HAS_PIE, true);
                if (err.isError()) return done();
                cpu(false, strips, 0);
                const BlitChain::Picture out{p.out, 1280, 720, 3};
                const auto               dma = [&](uint8_t *strip, uint16_t y0) {
                        const BlitChain::Picture from{strip, 1280, Rows, 2};
                        Error                    e;
                        for (uint16_t r = 0; r < Rows && e.isOk(); ++r) {
                                e = chain.add(from, 0, r, out, 0, uint16_t(2 * (y0 + r)), 1280, 1);
                                if (e.isOk()) e = chain.add(from, 0, r, out, 0, uint16_t(2 * (y0 + r) + 1), 1280, 1);
                        }
                        return e.isOk() ? chain.submit() : e;
                };
                // Rows written once into a picture of the source's height that the scan repeats
                // (Scanout's line mode): one 2D-DMA piece a strip.
                const BlitChain::Picture half{p.out, 1280, InH, 3};
                const auto               dmaOnce = [&](uint8_t *strip, uint16_t y0) {
                        const BlitChain::Picture from{strip, 1280, Rows, 2};
                        const Error              e = chain.add(from, 0, 0, half, 0, y0, 1280, Rows);
                        return e.isOk() ? chain.submit() : e;
                };
                const auto chainWait = [&] {
                        return chain.wait(JobTimeoutMs);
                };
                const auto pipelined = [&](bool once) {
                        Error e;
                        int   k = 0;
                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows, k ^= 1) {
                                uint8_t *strip = strips + size_t(k) * StripBytes;
                                cpu(BMDS_HAS_PIE, strip, y);                  // the other strip's job runs meanwhile
                                e = once ? dmaOnce(strip, y) : dma(strip, y); // waits for the job before
                        }
                        return e;
                };
                err = timeJob(
                        ctx, "dma_x2",
                        [&] {
                                Error e;
                                for (uint16_t y = 0; y < InH && e.isOk(); y += Rows) e = dma(strips, y);
                                return e;
                        },
                        chainWait);
                if (err.isOk()) err = timeJob(ctx, "pipelined_x2", [&] { return pipelined(false); }, chainWait);
                if (err.isOk())
                        err = timeJob(
                                ctx, "dma_x1",
                                [&] {
                                        Error e;
                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows) e = dmaOnce(strips, y);
                                        return e;
                                },
                                chainWait);
                if (err.isOk()) err = timeJob(ctx, "pipelined_x1", [&] { return pipelined(true); }, chainWait);
                if (err.isError()) return done();
                // The source's rows brought into internal RAM by the 2D-DMA (a chain of its own), the CPU
                // doubling from there: fetch k + 1, double k, widen k - 1 at once.
                const BlitChain::Picture source{p.src, InW, InH, 2};
                const auto               fetchStrip = [&](int k, uint16_t y0) {
                        const BlitChain::Picture to{fetched + size_t(k) * FetchBytes, InW, Rows, 2};
                        const Error              e = fetch.add(source, 0, y0, to, 0, 0, InW, Rows);
                        return e.isOk() ? fetch.submit() : e;
                };
                err = timeJob(
                        ctx, "fetch_to_internal",
                        [&] {
                                Error e;
                                for (uint16_t y = 0; y < InH && e.isOk(); y += Rows) e = fetchStrip(0, y);
                                return e;
                        },
                        [&] { return fetch.wait(JobTimeoutMs); });
                if (err.isError()) return done();
                const auto fromFetched = [&](int k, uint8_t *strip) {
                        const auto *in = reinterpret_cast<const uint16_t *>(fetched + size_t(k) * FetchBytes);
                        invalidate(in, FetchBytes); // the fetched rows are DMA's: the cache's old copy dropped first
                        for (uint16_t r = 0; r < Rows; ++r) {
                                auto *o = reinterpret_cast<uint16_t *>(strip + size_t(r) * InW * 4);
#if BMDS_HAS_PIE
                                double565Pie(in + size_t(r) * InW, o, InW);
#else
                                double565Scalar(in + size_t(r) * InW, o, InW);
#endif
                        }
                        writeBack(strip, StripBytes);
                        return;
                };
                err = timeJob(
                        ctx, "fetched_pipelined_x1",
                        [&] {
                                Error e = fetchStrip(0, 0);
                                int   k = 0;
                                for (uint16_t y = 0; y < InH && e.isOk(); y += Rows, k ^= 1) {
                                        e = fetch.wait(JobTimeoutMs);
                                        if (e.isOk() && y + Rows < InH) e = fetchStrip(k ^ 1, uint16_t(y + Rows));
                                        uint8_t *strip = strips + size_t(k) * StripBytes;
                                        if (e.isOk()) fromFetched(k, strip);
                                        if (e.isOk()) e = dmaOnce(strip, y);
                                }
                                return e;
                        },
                        chainWait);
                return done();
        }

        // A Surface's startBlit of small strips (an off-screen surface), and a cache write-back.
        Error runBlitCost(BenchContext &ctx) {
                Surface s;
                Error   err = s.start(1280, 720, Surface::Buffering::Double);
                auto   *strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 1280 * 3 * 8, 64));
                if (err.isOk() && strip == nullptr) err = Error::NoMem;
                if (err.isOk()) std::memset(strip, 0x40, 1280 * 3 * 8);
                const Rect clip[1] = {Rect{0, 0, 1280, 720}};
                for (int kind = 0; kind < 4 && err.isOk(); ++kind) {
                        const bool     wide = kind & 1;
                        const uint16_t w = (kind & 2) ? 1280 : 640;
                        char           metric[32];
                        std::snprintf(metric, sizeof(metric), "start_blit_%ux8_%s", unsigned(w), wide ? "565" : "888");
                        int i = 0;
                        err = timeJob(
                                ctx, metric,
                                [&] {
                                        const Rect to{0, uint16_t((i++ % 80) * 8), w, 8};
                                        return s.startBlit(
                                                to, strip, size_t(w) * (wide ? 2 : 3), Span<const Rect>(clip, 1),
                                                wide ? Surface::Source::Rgb565 : Surface::Source::Bgr24, 100);
                                },
                                [&] { return s.wait(100); });
                }
                if (err.isOk())
                        err = ctx.measure("msync_20k_internal", "us", BenchBetter::Lower, [&](double &out) {
                                Stopwatch watch;
                                (void)esp_cache_msync(strip, 1280 * 2 * 8,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                out = double(watch.elapsedUs());
                                return Error(Error::Ok);
                        });
                s.stop();
                Memory::free(strip);
                return err;
        }

        // ---- Surface 0, while nothing else has it ----

        // Display 0's surface, if it shows and nobody (the desktop) has it.
        Error freeSurface(BenchContext &ctx, Surface *&out) {
                auto [d, err] = Display::display(0);
                if (err.isError()) return err;
                if (!d->isRunning()) return Error::NotReady;
                if (d->owner() != nullptr) {
                        ctx.note("the display is the %s's: these run before the desktop starts", d->owner());
                        return Error::Busy;
                }
                out = &d->surface();
                return Error::Ok;
        }

        // The test picture (eight bars, a gray ramp) drawn and presented.
        Error runDraw(BenchContext &ctx) {
                Surface *s = nullptr;
                if (Error err = freeSurface(ctx, s); err.isError()) return err;
                return ctx.measure("test_picture_us", "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch   watch;
                        const Error err = detail::drawTestPicture(*s);
                        out = double(watch.elapsedUs());
                        return err;
                });
        }

        Error runFill(BenchContext &ctx) {
                Surface *s = nullptr;
                if (Error err = freeSurface(ctx, s); err.isError()) return err;
                const Rect  cases[] = {s->bounds(), Rect{100, 100, 64, 64}};
                const char *metrics[] = {"full_us", "square_64_us"};
                uint32_t    color = 0x204060;
                for (int i = 0; i < 2; ++i) {
                        const Error err = ctx.measure(metrics[i], "us", BenchBetter::Lower, [&](double &out) {
                                Stopwatch   watch;
                                const Error e = s->fill(cases[i], color ^= 0x102030, TimeoutMs);
                                out = double(watch.elapsedUs());
                                return e;
                        });
                        if (err.isError()) return err;
                }
                return s->present(TimeoutMs).error();
        }

        // A present waited for until shown (a double buffer swaps at a frame's start: a frame each).
        Error runPresent(BenchContext &ctx) {
                Surface *s = nullptr;
                if (Error err = freeSurface(ctx, s); err.isError()) return err;
                return ctx.measure("present_us", "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        auto [number, err] = s->present(TimeoutMs);
                        if (err.isOk()) err = s->waitShown(number, TimeoutMs);
                        out = double(watch.elapsedUs());
                        return err;
                });
        }

        // Squares bouncing on a flat background, drawn with fills and presented every frame from a
        // task of its own; then every pixel of the picture checked.
        class Anim {
                public:
                        static constexpr int MaxSquares = 64;

                        struct Stats {
                                        uint32_t frames = 0;
                                        uint32_t errors = 0;
                                        uint64_t drawUs = 0, presentUs = 0;
                        };

                        Error start(Surface &surface, int count) {
                                _surface = &surface;
                                _count = count;
                                _stats = Stats();
                                const int w = surface.width(), h = surface.height();
                                for (int i = 0; i < count; ++i) {
                                        Square &q = _squares[i];
                                        q.x = ((i * 97) % (w - Size)) & ~3;
                                        q.y = (i * 53) % (h - Size);
                                        q.dx = (4 + 4 * (i % 3)) * (i % 2 ? -1 : 1);
                                        q.dy = (1 + i % 5) * (i % 3 ? 1 : -1);
                                        q.px = q.x;
                                        q.py = q.y;
                                        q.color = Colors[i % 6];
                                }
                                Error err = surface.fill(surface.bounds(), Background, TimeoutMs);
                                for (int i = 0; i < count && err.isOk(); ++i) err = draw(_squares[i]);
                                if (err.isOk()) err = surface.present(TimeoutMs).error();
                                if (err.isError()) return err;
                                _stopping.store(false);
                                _exited.store(false);
                                Task::Config tc;
                                tc.name = "anim";
                                tc.stackBytes = 3072;
                                tc.priority = 5;
                                tc.core = 0;
                                return _task.start(tc, entry, this);
                        }

                        Stats stop() {
                                _stopping.store(true);
                                for (int i = 0; i < 200 && !_exited.load(); ++i) Task::sleep(1);
                                (void)_task.stop();
                                return _stats;
                        }

                        // The shown picture's pixels that aren't what the squares make.
                        uint32_t wrongPixels() const {
                                const Surface &s = *_surface;
                                const uint8_t *buffer = s.shown();
                                invalidate(buffer, s.stride() * s.height());
                                uint32_t wrong = 0;
                                for (int y = 0; y < s.height(); ++y) {
                                        const uint8_t *line = buffer + size_t(y) * s.stride();
                                        for (int x = 0; x < s.width(); ++x) {
                                                uint32_t want = Background;
                                                for (int i = 0; i < _count; ++i) {
                                                        const Square &q = _squares[i];
                                                        if (x >= q.px && x < q.px + Size && y >= q.py &&
                                                            y < q.py + Size)
                                                                want = q.color;
                                                }
                                                const uint8_t *p = line + 3 * x;
                                                if ((uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]) != want)
                                                        ++wrong;
                                        }
                                }
                                return wrong;
                        }

                private:
                        static constexpr int      Size = 64;
                        static constexpr uint32_t Background = 0x303030;
                        static constexpr uint32_t Colors[6] = {0xff4040, 0x40ff40, 0x4040ff,
                                                               0xffff40, 0x40ffff, 0xff40ff};

                        struct Square {
                                        int      x = 0, y = 0, dx = 0, dy = 0;
                                        int      px = 0, py = 0; // where it was drawn last
                                        uint32_t color = 0;
                        };

                        static void entry(void *arg) {
                                auto *self = static_cast<Anim *>(arg);
                                while (!self->_stopping.load()) self->frame();
                                self->_exited.store(true);
                                for (;;) Task::sleep(1000);
                        }

                        Error draw(const Square &q) {
                                return _surface->fill(
                                        Rect{uint16_t(q.x), uint16_t(q.y), uint16_t(Size), uint16_t(Size)}, q.color,
                                        TimeoutMs);
                        }

                        // One frame: every square's last place cleared, every square moved and drawn (in
                        // order: later ones on top), presented.
                        void frame() {
                                Stopwatch watch;
                                Surface  &s = *_surface;
                                for (int i = 0; i < _count; ++i) {
                                        const Square &q = _squares[i];
                                        if (s.fill(Rect{uint16_t(q.px), uint16_t(q.py), uint16_t(Size), uint16_t(Size)},
                                                   Background, TimeoutMs)
                                                    .isError())
                                                ++_stats.errors;
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        q.x += q.dx;
                                        q.y += q.dy;
                                        if (q.x < 0 || q.x > s.width() - Size) {
                                                q.dx = -q.dx;
                                                q.x += 2 * q.dx;
                                        }
                                        if (q.y < 0 || q.y > s.height() - Size) {
                                                q.dy = -q.dy;
                                                q.y += 2 * q.dy;
                                        }
                                        if (draw(q).isError()) ++_stats.errors;
                                        q.px = q.x;
                                        q.py = q.y;
                                }
                                const int64_t drawn = watch.elapsedUs();
                                if (s.present(TimeoutMs).isError()) ++_stats.errors;
                                const int64_t presented = watch.elapsedUs();
                                // A single buffer is never waited for: a frame's pace.
                                if (s.buffering() == Surface::Buffering::Single) {
                                        const uint32_t seen = s.frames();
                                        while (s.frames() == seen && !_stopping.load()) Task::sleep(1);
                                }
                                _stats.drawUs += uint64_t(drawn);
                                _stats.presentUs += uint64_t(presented - drawn);
                                ++_stats.frames;
                                return;
                        }

                        Surface          *_surface = nullptr;
                        Square            _squares[MaxSquares];
                        int               _count = 0;
                        Stats             _stats;
                        std::atomic<bool> _stopping{false};
                        std::atomic<bool> _exited{false};
                        Task              _task;
        };

        Error runAnim(BenchContext &ctx) {
                uint64_t squares = 0, seconds = 0;
                if (ctx.paramUnsigned("squares", squares).isError() || squares == 0 || squares > Anim::MaxSquares ||
                    ctx.paramUnsigned("seconds", seconds).isError() || seconds == 0 || seconds > 600)
                        return Error::Invalid;
                Surface *s = nullptr;
                if (Error err = freeSurface(ctx, s); err.isError()) return err;
                auto *anim = static_cast<Anim *>(Memory::alloc(MemSpace::Psram, sizeof(Anim)));
                if (anim == nullptr) return Error::NoMem;
                ::new (anim) Anim();
                const uint32_t runs = ctx.runs(), warmup = ctx.warmup();
                double         fps[4 * 16], *draw = fps + 16, *present = fps + 32;
                uint32_t       kept = 0, wrong = 0, errors = 0;
                Error          err;
                for (uint32_t r = 0; r < warmup + runs && err.isOk() && kept < 16; ++r) {
                        Stopwatch watch;
                        err = anim->start(*s, int(squares));
                        if (err.isError()) break;
                        for (uint64_t ms = 0; ms < seconds * 1000 && !ctx.isCancelled(); ms += 100) Task::sleep(100);
                        const Anim::Stats st = anim->stop();
                        const double      us = double(watch.elapsedUs());
                        Task::sleep(40); // the last present shown (two frames) before the picture is read
                        wrong += anim->wrongPixels();
                        errors += st.errors;
                        if (ctx.isCancelled()) err = Error::Cancelled;
                        if (r < warmup || st.frames == 0) continue;
                        fps[kept] = st.frames * 1e6 / us;
                        draw[kept] = double(st.drawUs) / st.frames;
                        present[kept] = double(st.presentUs) / st.frames;
                        ++kept;
                }
                anim->~Anim();
                Memory::free(anim);
                if (err.isError()) return err;
                ctx.reportSamples("frames_a_second", "/s", BenchBetter::Higher, Span<double>(fps, kept));
                ctx.reportSamples("draw_us", "us", BenchBetter::Lower, Span<double>(draw, kept));
                ctx.reportSamples("present_us", "us", BenchBetter::Lower, Span<double>(present, kept));
                if (errors != 0) ctx.note("%u drawing errors", unsigned(errors));
                if (wrong != 0) {
                        ctx.note("%u pixels wrong after the runs", unsigned(wrong));
                        return Error::CorruptData;
                }
                return errors != 0 ? Error::IoError : Error::Ok;
        }

} // namespace

namespace {

        // A full-screen L8 frame's conversion as the presenter does it (ProgramSurface::convert(),
        // devplan/bench.md §19): Doom's 320x200 expanded 3 times across through the palette into
        // BGR24 rows of a 30 KB strip in internal RAM, half of it at a time, each half written back
        // from the cache for the 2D-DMA; ms a frame. Without the write-back, with it, and the
        // picture in internal RAM instead of PSRAM.
        Error runExpand(BenchContext &ctx) {
                constexpr uint16_t Width = 320, Height = 200, Times = 3;
                constexpr size_t   StripBytes = 1280 * 3 * 8, Half = StripBytes / 2;
                constexpr size_t   RowBytes = size_t(Width) * Times * 3;
                constexpr uint16_t PerHalf = uint16_t(Half / RowBytes);
                auto *palette = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, 256 * sizeof(uint32_t)));
                auto *strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, StripBytes, 64));
                Error err = palette != nullptr && strip != nullptr ? Error(Error::Ok) : Error(Error::NoMem);
                for (uint32_t i = 0; err.isOk() && i < 256; ++i) palette[i] = i * 0x010203u;
                auto *table = static_cast<uint32_t *>(
                        Memory::alloc(MemSpace::Internal, Pixels::TripleTableWords * sizeof(uint32_t)));
                if (table == nullptr) err = Error::NoMem;
                if (err.isOk()) Pixels::makeTripleTable(palette, table);
                for (const MemSpace space : {MemSpace::Psram, MemSpace::Internal}) {
                        if (err.isError()) break;
                        auto *pic = static_cast<uint8_t *>(Memory::allocAligned(space, size_t(Width) * Height, 64));
                        if (pic == nullptr) {
                                err = Error::NoMem;
                                break;
                        }
                        for (size_t i = 0; i < size_t(Width) * Height; ++i) pic[i] = uint8_t(i * 7);
                        // Into the strip through the cache (then written back, as the presenter
                        // does), or past it (internal RAM's uncached alias: nothing to write back).
                        enum Way {
                                Expand,
                                WrittenBack,
                                Uncached,
                                Table // expandTriple(), written back
                        };
                        for (const Way way : {Expand, WrittenBack, Uncached, Table}) {
                                const bool writeBack = way == WrittenBack || way == Table;
                                uint8_t   *into =
                                        way == Uncached
                                                  ? reinterpret_cast<uint8_t *>(CACHE_LL_L2MEM_NON_CACHE_ADDR(strip))
                                                  : strip;
                                char metric[40];
                                std::snprintf(metric, sizeof(metric), "%s_%s_ms",
                                              space == MemSpace::Psram ? "psram" : "internal",
                                              way == Expand        ? "expand"
                                              : way == WrittenBack ? "written_back"
                                              : way == Table       ? "table"
                                                                   : "uncached");
                                err = ctx.measure(metric, "ms", BenchBetter::Lower, [&](double &out) {
                                        Stopwatch watch;
                                        int       k = 0;
                                        for (uint16_t y = 0; y < Height; y = uint16_t(y + PerHalf), k ^= 1) {
                                                const uint16_t rows =
                                                        uint16_t(Height - y < PerHalf ? Height - y : PerHalf);
                                                uint8_t *half = into + size_t(k) * Half;
                                                for (uint16_t r = 0; r < rows; ++r) {
                                                        const uint8_t *row = pic + size_t(y + r) * Width;
                                                        if (way == Table)
                                                                Pixels::expandTriple(row, Width, table,
                                                                                     half + size_t(r) * RowBytes);
                                                        else
                                                                Pixels::expandIndexed(row, Width, palette,
                                                                                      half + size_t(r) * RowBytes,
                                                                                      Times);
                                                }
                                                if (writeBack)
                                                        (void)esp_cache_msync(half, size_t(rows) * RowBytes,
                                                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                                                                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                        }
                                        out = double(watch.cycles()) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ / 1000.0;
                                        return Error(Error::Ok);
                                });
                                if (err.isError()) break;
                        }
                        Memory::free(pic);
                }
                Memory::free(strip);
                Memory::free(table);
                Memory::free(palette);
                return err;
        }

} // namespace

BMDS_BENCH(displayExpand, "display.expand", runExpand, BenchQuick,
           "A full-screen L8 frame converted as the presenter does: 320x200 to BGR24 at 3x (ms a frame)")
BMDS_BENCH_WITH(displayPpaScale, "display.ppa.scale", runPpaScale, BenchQuick, PpaParams,
                "The PPA's scaler and CLUT at the games' sizes, into a scratch picture (us a job)")
BMDS_BENCH_WITH(displayPpaSoak, "display.ppa.soak", runPpaSoak, 0, SoakParams,
                "640x360 RGB565 scaled by 2 to 1280x720 flat out: the PSRAM's write rate")
BMDS_BENCH_WITH(displayPpaPath, "display.ppa.path", runPixelPath, BenchQuick, PpaParams,
                "An integer scale's pixel path: the CPU doubling 565 rows, the 2D-DMA widening them (us a frame)")
BMDS_BENCH_WITH(displayBlitCost, "display.blitcost", runBlitCost, BenchQuick, PpaParams,
                "A surface's startBlit of 8-row strips, and a cache write-back (us each)")
BMDS_BENCH(displayDraw, "display.draw", runDraw, BenchScreen,
           "The test picture drawn and presented on surface 0 (before the desktop starts)")
BMDS_BENCH_WITH(displayFill, "display.fill", runFill, BenchScreen, PresentParams,
                "Fills on surface 0: the whole of it and a 64x64 square (before the desktop starts)")
BMDS_BENCH_WITH(displayPresent, "display.present", runPresent, BenchScreen, PresentParams,
                "A present on surface 0 waited for until shown (before the desktop starts)")
BMDS_BENCH_WITH(displayAnim, "display.anim", runAnim, BenchScreen, AnimParams,
                "Squares bouncing on surface 0, then every pixel checked (before the desktop starts)")

BMDS_NAMESPACE_END
