/**
 * @file      displayppa.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `display ppa`: the PPA's measurements for program surfaces (devplan/games.md §10b): the
 * scaler's and the CLUT's rates at the games' sizes, whether the scaler interpolates, and the
 * scan's counts meanwhile. Into a scratch picture in PSRAM, not the screen.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_cache.h>
#include <bmds/blitchain.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/memory.h>
#include <bmds/ppa.h>
#include <bmds/random.h>
#include <bmds/shell.h>
#include <bmds/simd.h>
#include <bmds/staticobject.h>
#include <bmds/surface.h>
#include <bmds/task.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t     OutWidth = 1280;
        constexpr uint16_t     OutHeight = 720;
        constexpr unsigned int JobTimeoutMs = 200;
        constexpr int          Runs = 20;

        // The pictures the cases use, PSRAM, freed together.
        struct Pictures {
                        uint8_t *out = nullptr; // OutWidth × OutHeight RGB888
                        uint8_t *src = nullptr; // up to 640 × 480 × 3, or 1280 × 720 × 3 for the copy
                        uint8_t *mid = nullptr; // 320 × 200 RGB888 (expanded, then scaled)
                        size_t   outBytes = size_t(OutWidth) * OutHeight * 3;
                        size_t   srcBytes = size_t(OutWidth) * OutHeight * 3;
                        size_t   midBytes = 320 * 200 * 3;

                        Error allocate() {
                                out = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, outBytes));
                                src = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, srcBytes));
                                mid = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, midBytes));
                                return out != nullptr && src != nullptr && mid != nullptr ? Error(Error::Ok)
                                                                                          : Error(Error::NoMem);
                        }

                        ~Pictures() {
                                Memory::free(out);
                                Memory::free(src);
                                Memory::free(mid);
                        }
        };

        void writeBack(const void *data, size_t bytes) {
                (void)esp_cache_msync(const_cast<void *>(data), (bytes + 63) & ~size_t(63),
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                return;
        }

        void invalidate(const void *data, size_t bytes) {
                (void)esp_cache_msync(const_cast<void *>(data), bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

        struct Timing {
                        uint64_t total = 0;
                        uint32_t min = UINT32_MAX;
                        uint32_t max = 0;
                        int      runs = 0;
                        Error    err;

                        void add(uint32_t us) {
                                total += us;
                                min = us < min ? us : min;
                                max = us > max ? us : max;
                                ++runs;
                                return;
                        }
        };

        void report(CommandContext &ctx, const char *name, const Timing &t, uint32_t outPixels) {
                if (t.err.isError()) {
                        ctx.printf("%-34s %s\n", name, t.err.desc());
                        return;
                }
                const uint32_t avg = static_cast<uint32_t>(t.total / uint64_t(t.runs));
                ctx.printf("%-34s %6u us avg, %6u min, %6u max  %5.1f Mpixel/s  %6u px\n", name, unsigned(avg),
                           unsigned(t.min), unsigned(t.max), avg != 0 ? double(outPixels) / avg : 0.0,
                           unsigned(outPixels));
                return;
        }

        // Runs @p job (starting one PPA job, or two in a row) Runs times, each waited for.
        template <typename Job> Timing measure(Ppa &ppa, Job job) {
                Timing t;
                for (int i = 0; i < Runs && t.err.isOk(); ++i) {
                        const uint64_t start = System::uptimeUs();
                        t.err = job();
                        if (t.err.isOk()) t.err = ppa.wait(JobTimeoutMs);
                        t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                }
                return t;
        }

        void fillRandom(uint8_t *data, size_t bytes) {
                Random::fill(data, bytes);
                writeBack(data, bytes);
                return;
        }

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


        // As measure(), for a 2D-DMA chain: the job's end waited for.
        template <typename Job> Timing measure2(BlitChain &chain, Job job) {
                Timing t;
                for (int i = 0; i < Runs && t.err.isOk(); ++i) {
                        const uint64_t start = System::uptimeUs();
                        t.err = job();
                        if (t.err.isOk()) t.err = chain.wait(JobTimeoutMs);
                        t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                }
                return t;
        }

        // Each RGB565 pixel of @p in twice into @p out (@p n pixels in).
        void double565Scalar(const uint16_t *in, uint16_t *out, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                        const uint32_t v = in[i];
                        reinterpret_cast<uint32_t *>(out)[i] = v | (v << 16);
                }
                return;
        }

#if BMDS_HAS_PIE
        // 8 pixels a step: loaded twice, zipped by 16 bits, stored (both 16-byte aligned, n a multiple of 8).
        void double565Pie(const uint16_t *in, uint16_t *out, size_t n) {
                const uint16_t *a = in;
                const uint16_t *b = in;
                uint16_t       *o = out;
                for (size_t i = 0; i < n; i += 8) {
                        asm volatile("esp.vld.128.ip q0, %[a], 16\n"
                                     "esp.vld.128.ip q1, %[b], 16\n"
                                     "esp.vzip.16 q0, q1\n"
                                     "esp.vst.128.ip q0, %[o], 16\n"
                                     "esp.vst.128.ip q1, %[o], 16\n"
                                     : [a] "+cr"(a), [b] "+cr"(b), [o] "+cr"(o)
                                     :
                                     : "memory");
                }
                return;
        }
#endif

        // The pixel path for an integer scale (Blocks: 640x360 RGB565 at 2x): the CPU doubles each line
        // into a strip in internal RAM, the 2D-DMA widens each strip row into two rows of RGB888.
        void runPixelPath(CommandContext &ctx, Pictures &p) {
                constexpr uint16_t InW = 640, InH = 360, Rows = 12;         // 360 = 30 strips
                constexpr size_t   StripBytes = size_t(InW) * 2 * 2 * Rows; // doubled width, RGB565
                auto *strips = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 2 * StripBytes, 64));
                BlitChain chain;
                Error     err = strips == nullptr ? Error(Error::NoMem) : Error(Error::Ok);
                if (err.isOk()) err = chain.start(BlitChain::Conversion::Rgb565ToRgb888, 2 * Rows);
                if (err.isError()) {
                        ctx.printf("pixel path: %s\n", err.desc());
                        Memory::free(strips);
                        return;
                }
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
                const BlitChain::Picture out{p.out, 1280, 720, 3};
                auto                     dma = [&](uint8_t *strip, uint16_t y0) {
                        const BlitChain::Picture from{strip, 1280, Rows, 2};
                        Error                    e;
                        for (uint16_t r = 0; r < Rows && e.isOk(); ++r) {
                                e = chain.add(from, 0, r, out, 0, uint16_t(2 * (y0 + r)), 1280, 1);
                                if (e.isOk()) e = chain.add(from, 0, r, out, 0, uint16_t(2 * (y0 + r) + 1), 1280, 1);
                        }
                        return e.isOk() ? chain.submit() : e;
                };
                for (bool pie : {false, true}) {
                        if (pie && !BMDS_HAS_PIE) continue;
                        Timing t;
                        for (int i = 0; i < Runs; ++i) {
                                const uint64_t start = System::uptimeUs();
                                for (uint16_t y = 0; y < InH; y += Rows) cpu(pie, strips, y);
                                t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                        }
                        report(ctx, pie ? "cpu: double 640x360 565 (PIE)" : "cpu: double 640x360 565 (C)", t,
                               1280 * 360);
                }
                {
                        // The same strip's source lines each time: in the cache, so the doubling and the
                        // write-back alone, without PSRAM reads.
                        Timing t;
                        for (int i = 0; i < Runs; ++i) {
                                const uint64_t start = System::uptimeUs();
                                for (uint16_t y = 0; y < InH; y += Rows) cpu(true, strips, 0);
                                t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                        }
                        report(ctx, "cpu: double, source cached (PIE)", t, 1280 * 360);
                }
                cpu(false, strips, 0);
                report(ctx, "dma: rows x2, 565 to 888 (alone)",
                       measure2(chain,
                                [&] {
                                        Error e;
                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows) e = dma(strips, y);
                                        return e;
                                }),
                       1280 * 720);
                report(ctx, "both, pipelined (Blocks frame)",
                       measure2(chain,
                                [&] {
                                        Error e;
                                        int   k = 0;
                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows, k ^= 1) {
                                                uint8_t *strip = strips + size_t(k) * StripBytes;
                                                cpu(true, strip, y); // the other strip's job runs meanwhile
                                                e = dma(strip, y);   // waits for the job before (its descriptors)
                                        }
                                        return e;
                                }),
                       1280 * 720);
                // Rows written once, into a picture of the source's height that the scan repeats
                // (Scanout line mode): one 2D-DMA piece a strip.
                const BlitChain::Picture half{p.out, 1280, InH, 3};
                auto                     dmaOnce = [&](uint8_t *strip, uint16_t y0) {
                        const BlitChain::Picture from{strip, 1280, Rows, 2};
                        const Error              e = chain.add(from, 0, 0, half, 0, y0, 1280, Rows);
                        return e.isOk() ? chain.submit() : e;
                };
                report(ctx, "dma: rows x1, 565 to 888 (alone)",
                       measure2(chain,
                                [&] {
                                        Error e;
                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows) e = dmaOnce(strips, y);
                                        return e;
                                }),
                       1280 * 360);
                report(ctx, "x1: cpu from PSRAM + dma, pipelined",
                       measure2(chain,
                                [&] {
                                        Error e;
                                        int   k = 0;
                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows, k ^= 1) {
                                                uint8_t *strip = strips + size_t(k) * StripBytes;
                                                cpu(true, strip, y);
                                                e = dmaOnce(strip, y);
                                        }
                                        return e;
                                }),
                       1280 * 360);
                // The source's rows brought into internal RAM by the 2D-DMA (a chain of its own), the
                // CPU doubling from there: fetch k + 1, double k, widen k - 1 at once.
                {
                        constexpr size_t FetchBytes = size_t(InW) * 2 * Rows;
                        auto            *fetched =
                                static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 2 * FetchBytes, 64));
                        BlitChain fetch;
                        Error     ferr = fetched == nullptr ? Error(Error::NoMem) : Error(Error::Ok);
                        if (ferr.isOk()) ferr = fetch.start(BlitChain::Conversion::None, 1);
                        if (ferr.isError()) {
                                ctx.printf("fetch: %s\n", ferr.desc());
                        } else {
                                const BlitChain::Picture source{p.src, InW, InH, 2};
                                auto                     fetchStrip = [&](int k, uint16_t y0) {
                                        const BlitChain::Picture to{fetched + size_t(k) * FetchBytes, InW, Rows, 2};
                                        const Error              e = fetch.add(source, 0, y0, to, 0, 0, InW, Rows);
                                        return e.isOk() ? fetch.submit() : e;
                                };
                                report(ctx, "fetch: 640x360 565 to internal (alone)",
                                       measure2(fetch,
                                                [&] {
                                                        Error e;
                                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows)
                                                                e = fetchStrip(0, y);
                                                        return e;
                                                }),
                                       InW * InH);
                                auto fromFetched = [&](int k, uint8_t *strip) {
                                        const auto *in =
                                                reinterpret_cast<const uint16_t *>(fetched + size_t(k) * FetchBytes);
                                        // The fetched rows are DMA's: the cache's old copy dropped first.
                                        invalidate(in, FetchBytes);
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
                                report(ctx, "x1: fetch + cpu + dma, pipelined",
                                       measure2(chain,
                                                [&] {
                                                        Error e = fetchStrip(0, 0);
                                                        int   k = 0;
                                                        for (uint16_t y = 0; y < InH && e.isOk(); y += Rows, k ^= 1) {
                                                                e = fetch.wait(JobTimeoutMs);
                                                                if (e.isOk() && y + Rows < InH)
                                                                        e = fetchStrip(k ^ 1, uint16_t(y + Rows));
                                                                uint8_t *strip = strips + size_t(k) * StripBytes;
                                                                if (e.isOk()) fromFetched(k, strip);
                                                                if (e.isOk()) e = dmaOnce(strip, y);
                                                        }
                                                        return e;
                                                }),
                                       1280 * 360);
                        }
                        fetch.stop();
                        Memory::free(fetched);
                }
                // The PIE kernel against the scalar one.
                uint32_t wrong = 0;
#if BMDS_HAS_PIE
                auto *a = reinterpret_cast<uint16_t *>(strips);
                auto *b = reinterpret_cast<uint16_t *>(strips + StripBytes);
                for (uint16_t y = 0; y < InH; ++y) {
                        double565Scalar(src + size_t(y) * InW, a, InW);
                        double565Pie(src + size_t(y) * InW, b, InW);
                        wrong += std::memcmp(a, b, size_t(InW) * 4) != 0 ? 1 : 0;
                }
#endif
                ctx.printf("PIE doubling against C: %u lines differ\n", unsigned(wrong));
                chain.stop();
                Memory::free(strips);
                return;
        }

        void runBench(CommandContext &ctx, Ppa &ppa, Pictures &p) {
                const Ppa::Picture out{p.out, OutWidth, OutHeight, Ppa::Format::Rgb888};
                fillRandom(p.src, p.srcBytes);
                {
                        const Ppa::Picture from{p.src, 640, 360, Ppa::Format::Rgb565};
                        report(ctx, "scale 565 640x360 x2 (Blocks)",
                               measure(ppa,
                                       [&] {
                                               return ppa.scale(from, Rect{0, 0, 640, 360}, out, 0, 0, 32, 32,
                                                                JobTimeoutMs);
                                       }),
                               1280 * 720);
                }
                {
                        const Ppa::Picture from{p.src, 320, 200, Ppa::Format::Rgb888};
                        report(ctx, "scale 888 320x200 x3/x3.5625 (Doom)",
                               measure(ppa,
                                       [&] {
                                               return ppa.scale(from, Rect{0, 0, 320, 200}, out, 160, 4, 48, 57,
                                                                JobTimeoutMs);
                                       }),
                               960 * 712);
                        report(ctx, "scale 888 320x200 x3 (integer)",
                               measure(ppa,
                                       [&] {
                                               return ppa.scale(from, Rect{0, 0, 320, 200}, out, 160, 60, 48, 48,
                                                                JobTimeoutMs);
                                       }),
                               960 * 600);
                }
                {
                        const Ppa::Picture from{p.src, 640, 480, Ppa::Format::Rgb565};
                        report(ctx, "scale 565 640x480 x1 (Solitaire)",
                               measure(ppa,
                                       [&] {
                                               return ppa.scale(from, Rect{0, 0, 640, 480}, out, 0, 0, 16, 16,
                                                                JobTimeoutMs);
                                       }),
                               640 * 480);
                }
                {
                        const Ppa::Picture from{p.src, 1280, 720, Ppa::Format::Rgb888};
                        report(ctx, "scale 888 1280x720 x1 (a copy)",
                               measure(ppa,
                                       [&] {
                                               return ppa.scale(from, Rect{0, 0, 1280, 720}, out, 0, 0, 16, 16,
                                                                JobTimeoutMs);
                                       }),
                               1280 * 720);
                }
                {
                        const Ppa::Picture from{p.src, 320, 200, Ppa::Format::L8};
                        const Ppa::Picture mid{p.mid, 320, 200, Ppa::Format::Rgb888};
                        for (bool bypass : {false}) {
                                report(ctx, bypass ? "expand L8 320x200 (bypass)" : "expand L8 320x200 (blend)",
                                       measure(ppa,
                                               [&] {
                                                       return ppa.expand(from, Rect{0, 0, 320, 200}, mid, 0, 0, bypass,
                                                                         JobTimeoutMs);
                                               }),
                                       320 * 200);
                        }
                        report(ctx, "expand + scale to 960x712 (Doom)",
                               measure(ppa,
                                       [&] {
                                               Error err = ppa.expand(from, Rect{0, 0, 320, 200}, mid, 0, 0, false,
                                                                      JobTimeoutMs);
                                               if (err.isOk()) err = ppa.wait(JobTimeoutMs);
                                               return err.isOk() ? ppa.scale(mid, Rect{0, 0, 320, 200}, out, 160, 4, 48,
                                                                             57, JobTimeoutMs)
                                                                 : err;
                                       }),
                               960 * 712);
                }
                checkInterpolation(ctx, ppa, p, 32);
                checkInterpolation(ctx, ppa, p, 24);
                checkExpand(ctx, ppa, p, false);
                runPixelPath(ctx, p);
                return;
        }

        // Scales 640x360 RGB565 by 2 into the scratch picture for @p seconds, flat out.
        void runSoak(CommandContext &ctx, Ppa &ppa, Pictures &p, unsigned seconds) {
                fillRandom(p.src, p.srcBytes);
                const Ppa::Picture from{p.src, 640, 360, Ppa::Format::Rgb565};
                const Ppa::Picture out{p.out, OutWidth, OutHeight, Ppa::Format::Rgb888};
                const uint64_t     start = System::uptimeUs();
                uint32_t           jobs = 0;
                Error              err;
                while (err.isOk() && System::uptimeUs() - start < uint64_t(seconds) * 1'000'000) {
                        err = ppa.scale(from, Rect{0, 0, 640, 360}, out, 0, 0, 32, 32, JobTimeoutMs);
                        if (err.isOk()) err = ppa.wait(JobTimeoutMs);
                        ++jobs;
                }
                const uint64_t us = System::uptimeUs() - start;
                ctx.printf("soak: %u frames of 1280x720 in %u ms (%.1f a second, %.0f MB/s written)%s%s\n",
                           unsigned(jobs), unsigned(us / 1000), jobs * 1e6 / double(us),
                           jobs * 1280.0 * 720 * 3 / double(us), err.isError() ? ": " : "",
                           err.isError() ? err.desc() : "");
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

        // display blitcost: a Surface's startBlit of small strips, timed (an off-screen surface).
        int runBlitCost(CommandContext &ctx) {
                Surface s;
                Error   err = s.start(1280, 720, Surface::Buffering::Double);
                auto   *strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, 1280 * 3 * 8, 64));
                if (err.isError() || strip == nullptr) {
                        ctx.printf("blitcost: %s\n", err.isError() ? err.desc() : "no memory");
                        Memory::free(strip);
                        return Shell::ExitFailure;
                }
                std::memset(strip, 0x40, 1280 * 3 * 8);
                const Rect clip[1] = {Rect{0, 0, 1280, 720}};
                for (int kind = 0; kind < 4; ++kind) {
                        const bool     wide = kind & 1;
                        const uint16_t w = (kind & 2) ? 1280 : 640;
                        Timing         t;
                        for (int i = 0; i < 100 && t.err.isOk(); ++i) {
                                const Rect     to{0, uint16_t((i % 80) * 8), w, 8};
                                const uint64_t start = System::uptimeUs();
                                t.err = s.startBlit(to, strip, size_t(w) * (wide ? 2 : 3), Span<const Rect>(clip, 1),
                                                    wide ? Surface::Source::Rgb565 : Surface::Source::Bgr24, 100);
                                if (t.err.isOk()) t.err = s.wait(100);
                                t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                        }
                        char name[48];
                        std::snprintf(name, sizeof(name), "startBlit %ux8 %s", unsigned(w), wide ? "565" : "888");
                        report(ctx, name, t, uint32_t(w) * 8);
                }
                {
                        Timing t;
                        for (int i = 0; i < 100; ++i) {
                                const uint64_t start = System::uptimeUs();
                                (void)esp_cache_msync(strip, 1280 * 2 * 8,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                t.add(static_cast<uint32_t>(System::uptimeUs() - start));
                        }
                        report(ctx, "msync 20 KB internal", t, 0);
                }
                s.stop();
                Memory::free(strip);
                return Shell::ExitSuccess;
        }
} // namespace

namespace detail {

        // display ppa [soak SECONDS]
        int runDisplayPpa(CommandContext &ctx) {
                const auto args = ctx.args();
                unsigned   soak = 0;
                if (args.size() == 3 && std::strcmp(args[2], "blitcost") == 0) return runBlitCost(ctx);
                const bool pathOnly = args.size() == 3 && std::strcmp(args[2], "path") == 0;
                if (pathOnly) {
                        // runPixelPath() alone, below.
                } else if (args.size() == 4 && std::strcmp(args[2], "soak") == 0) {
                        soak = static_cast<unsigned>(std::atoi(args[3]));
                        if (soak == 0) return ctx.usage();
                } else if (args.size() != 2) {
                        return ctx.usage();
                }
                Pictures pictures;
                Error    err = pictures.allocate();
                Ppa      ppa;
                if (err.isOk()) err = ppa.start();
                if (err.isError()) {
                        ctx.eprintf("display: ppa: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                auto [d, derr] = Display::display(0);
                const bool           scanning = derr.isOk() && d->isRunning();
                const Scanout::Stats before = scanning ? d->scanout().stats() : Scanout::Stats{};
                if (pathOnly) {
                        runPixelPath(ctx, pictures);
                } else if (soak != 0) {
                        runSoak(ctx, ppa, pictures, soak);
                } else {
                        runBench(ctx, ppa, pictures);
                }
                if (scanning) {
                        const Scanout::Stats after = d->scanout().stats();
                        ctx.printf("scan meanwhile: %u frames, %u underruns, %u stalls, %u resyncs\n",
                                   unsigned(after.frames - before.frames), unsigned(after.underruns - before.underruns),
                                   unsigned(after.stalls - before.stalls), unsigned(after.resyncs - before.resyncs));
                }
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
