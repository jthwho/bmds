/**
 * @file      simdcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `simd` command (devplan/simd.md §3): each vector kernel checked byte for byte against its
 * scalar twin on random input; `stress` does that from tasks on both cores switching mid-kernel,
 * and checks PIE's registers survive the switches (simd.md §5). The kernels' timings are the
 * `simd.*` benchmarks (devplan/bench.md). Its output format is a userspace interface: keep the
 * lines and their fields, add new ones only at the end.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <bmds/bench.h>
#include <bmds/command.h>
#include <bmds/bitmapfont.h>
#include <bmds/glyphrenderer.h>
#include <bmds/image.h>
#include <bmds/imagescaler.h>
#include <bmds/images.h>
#include <bmds/memory.h>
#include <bmds/softjpeg.h>
#include <bmds/pngfilter.h>
#include <bmds/shell.h>
#include <bmds/simd.h>
#include <bmds/stopwatch.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/textblend.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Deterministic noise (xorshift32): a failing case can be found again.
        struct Noise {
                        uint32_t s = 0x12345678u;
                        uint32_t next() {
                                s ^= s << 13;
                                s ^= s >> 17;
                                s ^= s << 5;
                                return s;
                        }
                        uint32_t below(uint32_t n) { return next() % n; }
                        void     fill(uint8_t *p, size_t n) {
                                for (size_t i = 0; i < n; ++i) p[i] = uint8_t(next() >> 24);
                        }
        };

        // A buffer from @p space, freed with it.
        struct Block {
                        uint8_t *p = nullptr;
                        Block(MemSpace space, size_t bytes) {
                                p = static_cast<uint8_t *>(Memory::allocAligned(space, bytes, 16));
                        }
                        ~Block() { Memory::free(p); }
        };

        // Times @p work (one call: `work(kernel)`) with the scalar kernel and, where the build has
        // them, the vector one: metrics `PREFIXscalar_UNIT` and `PREFIXpie_UNIT`, each sample the
        // time divided by @p per (items a call) in @p unit ("ms" or "us").
        template <typename Work>
        Error scalarAndPie(BenchContext &ctx, const char *prefix, const char *unit, double per, Work work) {
                const double divide = (unit[0] == 'm' ? 1000.0 : 1.0) * per;
                for (const Simd::Kernel k : {Simd::Kernel::Scalar, Simd::Kernel::Pie}) {
                        if (k == Simd::Kernel::Pie && !Simd::hasPie()) continue;
                        char metric[BenchContext::MaxMetricName + 1];
                        std::snprintf(metric, sizeof(metric), "%s%s_%s", prefix,
                                      k == Simd::Kernel::Scalar ? "scalar" : "pie", unit);
                        const Error err = ctx.measure(metric, unit, BenchBetter::Lower, [&](double &out) {
                                Stopwatch watch;
                                work(k);
                                out = double(watch.elapsedUs()) / divide;
                                return Error(Error::Ok);
                        });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        const char *formatName(PixelFormat f) {
                return f == PixelFormat::Bgr24 ? "bgr24" : f == PixelFormat::Bgra32 ? "bgra32" : "gray8";
        }

        // ---- The kernels: each a test (random cases, both ways compared) and a bench.

        int testScale(CommandContext &ctx, Noise &noise, unsigned cases) {
                static constexpr PixelFormat Formats[] = {PixelFormat::Bgr24, PixelFormat::Bgra32, PixelFormat::Gray8};
                for (unsigned c = 0; c < cases; ++c) {
                        const PixelFormat f = Formats[noise.below(3)];
                        const unsigned    bpp = bytesPerPixel(f);
                        ImageBuffer       in, a, b;
                        in.width = 1 + noise.below(300);
                        in.height = 1 + noise.below(200);
                        in.format = a.format = b.format = f;
                        in.stride = size_t(in.width) * bpp + noise.below(9); // any stride, any alignment
                        a.width = b.width = 1 + noise.below(300);
                        a.height = b.height = 1 + noise.below(200);
                        a.stride = b.stride = size_t(a.width) * bpp;
                        const size_t offset = noise.below(16);
                        Block src(MemSpace::Psram, in.stride * in.height + 32), x(MemSpace::Psram, a.stride * a.height),
                                y(MemSpace::Psram, b.stride * b.height);
                        if (src.p == nullptr || x.p == nullptr || y.p == nullptr) {
                                ctx.eprintf("simd: out of memory\n");
                                return Shell::ExitFailure;
                        }
                        noise.fill(src.p, in.stride * in.height + 32);
                        in.pixels = src.p + offset;
                        a.pixels = x.p;
                        b.pixels = y.p;
                        (void)ImageScaler::scale(in, a, MemSpace::Psram, Simd::Kernel::Scalar);
                        (void)ImageScaler::scale(in, b, MemSpace::Psram, Simd::Kernel::Pie);
                        if (std::memcmp(x.p, y.p, a.stride * a.height) != 0) {
                                size_t at = 0;
                                while (x.p[at] == y.p[at]) ++at;
                                ctx.printf("scale: case %u (%s %ux%u+%u to %ux%u) differs at byte %u: %u, %u\n", c,
                                           formatName(f), unsigned(in.width), unsigned(in.height), unsigned(offset),
                                           unsigned(a.width), unsigned(a.height), unsigned(at), x.p[at], y.p[at]);
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("scale: %u cases, the same\n", cases);
                return Shell::ExitSuccess;
        }

        // Glyphs: random cells (characters, colours, bold, underline) drawn both ways.
        constexpr uint16_t GlyphStripWidth = 640;

        void drawCells(GlyphRenderer &r, Noise &noise, uint8_t *out, size_t stride) {
                TextCell cells[GlyphStripWidth / 8];
                for (TextCell &c : cells) {
                        c.codepoint = 0x20 + noise.below(0x5f);
                        c.fg = noise.next() & 0xffffff;
                        c.bg = noise.next() & 0xffffff;
                        c.flags = uint8_t(noise.below(4));
                }
                size_t used = 0;
                (void)r.drawCells(Span<const TextCell>(cells, GlyphStripWidth / 8), Fonts::unscii16, out, stride,
                                  GlyphStripWidth, used);
                return;
        }

        int testGlyphs(CommandContext &ctx, unsigned cases) {
                const size_t  stride = size_t(GlyphStripWidth) * 3;
                const size_t  bytes = stride * Fonts::unscii16.height();
                Block         a(MemSpace::Internal, bytes), b(MemSpace::Internal, bytes);
                GlyphRenderer scalar(GlyphRenderer::Kernel::Scalar), pie(GlyphRenderer::Kernel::Pie);
                if (a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                for (unsigned c = 0; c < cases; ++c) {
                        Noise na, nb;
                        na.s = nb.s = 0x9e3779b9u + c;
                        drawCells(scalar, na, a.p, stride);
                        drawCells(pie, nb, b.p, stride);
                        if (std::memcmp(a.p, b.p, bytes) != 0) {
                                ctx.printf("glyphs: case %u differs\n", c);
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("glyphs: %u cases, the same\n", cases);
                return Shell::ExitSuccess;
        }

        int testSwap24(CommandContext &ctx, Noise &noise) {
                Block in(MemSpace::Internal, 3 * 512 + 32), a(MemSpace::Internal, 3 * 512 + 32),
                        b(MemSpace::Internal, 3 * 512 + 32);
                if (in.p == nullptr || a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t n = noise.below(512), io = noise.below(16), oo = noise.below(16);
                        noise.fill(in.p, 3 * 512 + 32);
                        std::memset(a.p, 0x5a, 3 * 512 + 32);
                        std::memset(b.p, 0x5a, 3 * 512 + 32);
                        Pixels::swap24(in.p + io, a.p + oo, n, Simd::Kernel::Scalar);
                        Pixels::swap24(in.p + io, b.p + oo, n, Simd::Kernel::Pie);
                        if (std::memcmp(a.p, b.p, 3 * 512 + 32) != 0) {
                                ctx.printf("swap24: case %u (%u pixels, +%u to +%u) differs\n", c, unsigned(n),
                                           unsigned(io), unsigned(oo));
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("swap24: 2000 cases, the same\n");
                return Shell::ExitSuccess;
        }

        // Alpha as icons have it: mostly clear or opaque, the edges between.
        void iconPixels(Noise &noise, uint32_t *p, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                        const uint32_t kind = noise.below(10);
                        const uint32_t a = kind < 4 ? 0 : kind < 8 ? 0xff : noise.below(256);
                        p[i] = a << 24 | (noise.next() & 0xffffff);
                }
                return;
        }

        int testBlend(CommandContext &ctx, Noise &noise) {
                constexpr size_t Most = 600;
                Block            in(MemSpace::Psram, 4 * Most + 64), a(MemSpace::Internal, 3 * Most + 32),
                        b(MemSpace::Internal, 3 * Most + 32);
                if (in.p == nullptr || a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                auto *pixels = reinterpret_cast<uint32_t *>(in.p);
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t      n = noise.below(Most), io = noise.below(16), oo = noise.below(16);
                        const uint32_t    bg = noise.next() & 0xffffff;
                        const PixelFormat f = noise.below(2) == 0 ? PixelFormat::Bgr24 : PixelFormat::Rgb24;
                        iconPixels(noise, pixels, Most + 16);
                        std::memset(a.p, 0x5a, 3 * Most + 32);
                        std::memset(b.p, 0x5a, 3 * Most + 32);
                        Pixels::fromArgb(pixels + io, n, a.p + oo, f, bg, Simd::Kernel::Scalar);
                        Pixels::fromArgb(pixels + io, n, b.p + oo, f, bg, Simd::Kernel::Pie);
                        if (std::memcmp(a.p, b.p, 3 * Most + 32) != 0) {
                                size_t at = 0;
                                while (a.p[at] == b.p[at]) ++at;
                                ctx.printf("blend: case %u (%u pixels, +%u to +%u, %s) differs at byte %u: %u, %u\n", c,
                                           unsigned(n), unsigned(io), unsigned(oo),
                                           f == PixelFormat::Bgr24 ? "bgr24" : "rgb24", unsigned(at), a.p[at], b.p[at]);
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("blend: 2000 cases, the same\n");
                return Shell::ExitSuccess;
        }

        // Text's blend (devplan/fonts.md §4d): one color by alpha (0 and 255 among the rest) over
        // B, G, R, R, G, B or grey, any length and alignment.
        int testText(CommandContext &ctx, Noise &noise) {
                constexpr size_t Most = 600;
                Block            alpha(MemSpace::Internal, Most + 16), a(MemSpace::Internal, 3 * Most + 32),
                        b(MemSpace::Internal, 3 * Most + 32);
                if (alpha.p == nullptr || a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                static constexpr TextBlend::Format Formats[] = {TextBlend::Format::Bgr24, TextBlend::Format::Rgb24,
                                                                TextBlend::Format::Gray8};
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t            n = noise.below(Most), ao = noise.below(16), po = noise.below(16);
                        const uint32_t          color = noise.next() & 0xffffff;
                        const TextBlend::Format f = Formats[noise.below(3)];
                        for (size_t i = 0; i < Most + 16; ++i) {
                                const uint32_t k = noise.below(4);
                                alpha.p[i] = k == 0 ? 0 : k == 1 ? 255 : uint8_t(noise.next() >> 24);
                        }
                        noise.fill(a.p, 3 * Most + 32);
                        std::memcpy(b.p, a.p, 3 * Most + 32);
                        TextBlend::blend(a.p + po, f, alpha.p + ao, unsigned(n), color, Simd::Kernel::Scalar);
                        TextBlend::blend(b.p + po, f, alpha.p + ao, unsigned(n), color, Simd::Kernel::Pie);
                        if (std::memcmp(a.p, b.p, 3 * Most + 32) != 0) {
                                size_t at = 0;
                                while (a.p[at] == b.p[at]) ++at;
                                ctx.printf(
                                        "text: case %u (%u pixels, format %u, +%u, +%u) differs at byte %u: %u, %u\n",
                                        c, unsigned(n), unsigned(f), unsigned(ao), unsigned(po), unsigned(at), a.p[at],
                                        b.p[at]);
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("text: 2000 cases, the same\n");
                return Shell::ExitSuccess;
        }

        // PNG's filters: rows of a smooth picture with noise (so each filter wins somewhere), any length.
        void pngRows(Noise &noise, uint8_t *prev, uint8_t *cur, size_t bytes) {
                const unsigned kind = noise.below(4);
                for (size_t i = 0; i < bytes; ++i) {
                        prev[i] = uint8_t(i / 3 * 2 + noise.below(kind == 0 ? 2 : 40));
                        cur[i] = kind == 1 ? prev[i] : uint8_t(prev[i] + (kind == 2 ? 3 : noise.below(256)));
                }
                return;
        }

        int testPngFilter(CommandContext &ctx, Noise &noise) {
                constexpr size_t Most = 3 * 700;
                Block            buf(MemSpace::Internal, 2 * (Most + 16) + 2 * (Most + 1));
                if (buf.p == nullptr) return Shell::ExitFailure;
                uint8_t *prev = buf.p + 16, *cur = prev + Most + 16, *a = cur + Most, *b = a + Most + 1;
                std::memset(buf.p, 0, 2 * (Most + 16));
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t bytes = 3 * (1 + noise.below(700));
                        pngRows(noise, prev, cur, bytes);
                        PngFilter::row(cur, prev, bytes, a, Simd::Kernel::Scalar);
                        PngFilter::row(cur, prev, bytes, b, Simd::Kernel::Pie);
                        if (std::memcmp(a, b, bytes + 1) != 0) {
                                ctx.printf("pngfilter: case %u (%u bytes) differs: filters %u, %u\n", c,
                                           unsigned(bytes), a[0], b[0]);
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("pngfilter: 2000 rows, the same\n");
                return Shell::ExitSuccess;
        }

        // TJpgDec's output: JPEGs made by the codec (every sampling, odd sizes), decoded both ways at
        // every reduction, into B, G, R and R, G, B.
        struct Bytes {
                        uint8_t *p = nullptr;
                        size_t   n = 0, room = 0;
        };

        Error toBytes(void *arg, ByteSpan data) {
                auto *b = static_cast<Bytes *>(arg);
                if (b->n + data.size() > b->room) return Error::BufferTooSmall;
                std::memcpy(b->p + b->n, data.data(), data.size());
                b->n += data.size();
                return Error::Ok;
        }

        bool makeJpeg(Noise &noise, uint32_t w, uint32_t h, JpegHeader::Sampling sampling, Bytes &jpeg) {
                Block pic(MemSpace::Psram, size_t(w) * h * 3);
                if (pic.p == nullptr) return false;
                const uint32_t phase = noise.next();
                for (uint32_t yy = 0; yy < h; ++yy)
                        for (uint32_t xx = 0; xx < w; ++xx) {
                                uint8_t *p = pic.p + (size_t(yy) * w + xx) * 3;
                                p[0] = uint8_t(xx * 3 + phase);
                                p[1] = uint8_t(yy * 5 + (phase >> 8));
                                p[2] = uint8_t((xx + yy) * 2 + noise.below(24));
                        }
                ImageBuffer in;
                in.pixels = pic.p;
                in.width = w;
                in.height = h;
                in.stride = size_t(w) * 3;
                Images::Encode how;
                how.codec = ImageCodec::Jpeg;
                how.quality = 85;
                how.sampling = sampling;
                jpeg.n = 0;
                return Images::encode(in, how, toBytes, &jpeg).isOk();
        }

        int testSoftJpeg(CommandContext &ctx, Noise &noise) {
                static constexpr JpegHeader::Sampling Samplings[] = {
                        JpegHeader::Sampling::Yuv420, JpegHeader::Sampling::Yuv422, JpegHeader::Sampling::Yuv444,
                        JpegHeader::Sampling::Gray};
                Bytes jpeg;
                jpeg.room = 1024 * 1024;
                Block store(MemSpace::Psram, jpeg.room);
                jpeg.p = store.p;
                if (jpeg.p == nullptr) return Shell::ExitFailure;
                unsigned cases = 0;
                for (unsigned c = 0; c < 24; ++c) {
                        const uint32_t w = 16 * (1 + noise.below(20)), h = 16 * (1 + noise.below(15));
                        const auto     sampling = Samplings[c % 4];
                        if (!makeJpeg(noise, w, h, sampling, jpeg)) {
                                ctx.printf("softjpeg: couldn't make a %ux%u JPEG\n", unsigned(w), unsigned(h));
                                return Shell::ExitFailure;
                        }
                        for (unsigned shift = 0; shift <= SoftJpeg::MaxShift; ++shift)
                                for (const PixelFormat f : {PixelFormat::Bgr24, PixelFormat::Rgb24}) {
                                        ImageBuffer a, b;
                                        a.width = b.width = SoftJpeg::reduced(w, shift);
                                        a.height = b.height = SoftJpeg::reduced(h, shift);
                                        a.stride = b.stride = size_t(a.width) * 3;
                                        a.format = b.format = f;
                                        Block x(MemSpace::Psram, a.stride * a.height),
                                                y(MemSpace::Psram, b.stride * b.height);
                                        a.pixels = x.p;
                                        b.pixels = y.p;
                                        const ByteSpan file(jpeg.p, jpeg.n);
                                        const Error    e1 = SoftJpeg::decode(file, a, shift, MemSpace::Internal,
                                                                             Simd::Kernel::Scalar);
                                        const Error    e2 =
                                                SoftJpeg::decode(file, b, shift, MemSpace::Internal, Simd::Kernel::Pie);
                                        ++cases;
                                        if (e1.isError() || e2.isError() ||
                                            std::memcmp(x.p, y.p, a.stride * a.height) != 0) {
                                                size_t at = 0;
                                                while (at < a.stride * a.height && x.p[at] == y.p[at]) ++at;
                                                ctx.printf("softjpeg: %ux%u sampling %u 1/%u %s differs at byte %u "
                                                           "(%s, %s)\n",
                                                           unsigned(w), unsigned(h), unsigned(sampling), 1u << shift,
                                                           f == PixelFormat::Bgr24 ? "bgr24" : "rgb24", unsigned(at),
                                                           e1.desc(), e2.desc());
                                                return Shell::ExitFailure;
                                        }
                                }
                }
                ctx.printf("softjpeg: %u decodes, the same\n", cases);
                return Shell::ExitSuccess;
        }


        // repeat16 at 2 (the PIE case: aligned, whole steps and a scalar tail).
        int testRepeat16(CommandContext &ctx, Noise &noise) {
                constexpr size_t Max = 1024;
                Block            in(MemSpace::Internal, Max * 2), a(MemSpace::Internal, Max * 4 + 32),
                        b(MemSpace::Internal, Max * 4 + 32);
                if (in.p == nullptr || a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t n = noise.below(Max);
                        noise.fill(in.p, Max * 2);
                        std::memset(a.p, 0x5a, Max * 4 + 32);
                        std::memset(b.p, 0x5a, Max * 4 + 32);
                        const auto *src = reinterpret_cast<const uint16_t *>(in.p);
                        Pixels::repeat16(src, n, reinterpret_cast<uint16_t *>(a.p), 2, Simd::Kernel::Scalar);
                        Pixels::repeat16(src, n, reinterpret_cast<uint16_t *>(b.p), 2, Simd::Kernel::Pie);
                        if (std::memcmp(a.p, b.p, Max * 4 + 32) != 0) {
                                ctx.printf("repeat16: case %u (%u pixels) differs\n", c, unsigned(n));
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("repeat16: 2000 cases, the same\n");
                return Shell::ExitSuccess;
        }

        // repeat8 at 2 (the PIE case: aligned, whole steps and a scalar tail; demo.md §8a).
        int testRepeat8(CommandContext &ctx, Noise &noise) {
                constexpr size_t Max = 2048;
                Block            in(MemSpace::Internal, Max), a(MemSpace::Internal, Max * 2 + 32),
                        b(MemSpace::Internal, Max * 2 + 32);
                if (in.p == nullptr || a.p == nullptr || b.p == nullptr) return Shell::ExitFailure;
                for (unsigned c = 0; c < 2000; ++c) {
                        const size_t n = noise.below(Max);
                        noise.fill(in.p, Max);
                        std::memset(a.p, 0x5a, Max * 2 + 32);
                        std::memset(b.p, 0x5a, Max * 2 + 32);
                        Pixels::repeat8(in.p, n, a.p, 2, Simd::Kernel::Scalar);
                        Pixels::repeat8(in.p, n, b.p, 2, Simd::Kernel::Pie);
                        if (std::memcmp(a.p, b.p, Max * 2 + 32) != 0) {
                                ctx.printf("repeat8: case %u (%u pixels) differs\n", c, unsigned(n));
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("repeat8: 2000 cases, the same\n");
                return Shell::ExitSuccess;
        }

        // ---- Stress (simd.md §5): the kernels from tasks that switch mid-kernel, and PIE's registers
        // checked across switches.

        constexpr unsigned StressWorkers = 4; // two a core, at one priority: switched every tick
        constexpr unsigned StressKinds = 5;
        const char *const  StressNames[StressKinds] = {"registers", "swap24", "repeat16", "pngfilter", "blend"};

        struct StressWorker {
                        Task               task;
                        Noise              noise;
                        std::atomic<bool> *stop = nullptr;
                        std::atomic<bool>  done{false};
                        uint8_t           *buf = nullptr; // StressBytes, 16-byte aligned, internal RAM
                        uint32_t           cases[StressKinds] = {};
                        uint32_t           failures[StressKinds] = {};
                        uint32_t           firstKind = StressKinds; // none yet
                        uint32_t           firstCase = 0;
                        uint32_t           firstDetail = 0;
                        unsigned           id = 0;
        };

        constexpr size_t StressBytes = 3 * 4096;

        // PIE's registers (all FreeRTOS saves: Q0-Q7, QACC, UA_STATE, XACC, SAR, SAR_BYTES) loaded with
        // noise and read straight back, then read again after many yields to a task using PIE too: the
        // two reads must match. (Read back, not compared with the noise: some bits may not be kept.)
        // @return the first byte that differs, or -1.
        int32_t stressRegisters(StressWorker &w) {
#if BMDS_HAS_PIE
                constexpr size_t Bytes = 8 * 16 + 4 * 16 + 16 + 16; // Q, QACC, UA_STATE, XACC and the SARs
                uint8_t         *pattern = w.buf, *before = w.buf + 256, *after = w.buf + 512;
                w.noise.fill(pattern, Bytes);
                std::memset(before, 0, Bytes);
                std::memset(after, 0, Bytes);
                const uint32_t sar = w.noise.below(64), sarBytes = w.noise.below(16);
                uint8_t       *p = pattern, *o = before;
                asm volatile("esp.vld.128.ip q0, %[p], 16\n"
                             "esp.vld.128.ip q1, %[p], 16\n"
                             "esp.vld.128.ip q2, %[p], 16\n"
                             "esp.vld.128.ip q3, %[p], 16\n"
                             "esp.vld.128.ip q4, %[p], 16\n"
                             "esp.vld.128.ip q5, %[p], 16\n"
                             "esp.vld.128.ip q6, %[p], 16\n"
                             "esp.vld.128.ip q7, %[p], 16\n"
                             "esp.ld.qacc.l.l.128.ip %[p], 16\n"
                             "esp.ld.qacc.l.h.128.ip %[p], 16\n"
                             "esp.ld.qacc.h.l.128.ip %[p], 16\n"
                             "esp.ld.qacc.h.h.128.ip %[p], 16\n"
                             "esp.ld.ua.state.ip %[p], 16\n"
                             "esp.ld.xacc.ip %[p], 0\n"
                             "esp.movx.w.sar %[sar]\n"
                             "esp.movx.w.sar.bytes %[sb]\n"
                             : [p] "+r"(p)
                             : [sar] "r"(sar), [sb] "r"(sarBytes)
                             : "memory");
                const auto store = [](uint8_t *to) {
                        uint32_t r0, r1;
                        asm volatile("esp.vst.128.ip q0, %[o], 16\n"
                                     "esp.vst.128.ip q1, %[o], 16\n"
                                     "esp.vst.128.ip q2, %[o], 16\n"
                                     "esp.vst.128.ip q3, %[o], 16\n"
                                     "esp.vst.128.ip q4, %[o], 16\n"
                                     "esp.vst.128.ip q5, %[o], 16\n"
                                     "esp.vst.128.ip q6, %[o], 16\n"
                                     "esp.vst.128.ip q7, %[o], 16\n"
                                     "esp.st.qacc.l.l.128.ip %[o], 16\n"
                                     "esp.st.qacc.l.h.128.ip %[o], 16\n"
                                     "esp.st.qacc.h.l.128.ip %[o], 16\n"
                                     "esp.st.qacc.h.h.128.ip %[o], 16\n"
                                     "esp.st.ua.state.ip %[o], 16\n"
                                     "esp.st.u.xacc.ip %[o], 0\n"
                                     "esp.movx.r.sar %[r0]\n"
                                     "esp.movx.r.sar.bytes %[r1]\n"
                                     : [o] "+r"(to), [r0] "=r"(r0), [r1] "=r"(r1)
                                     :
                                     : "memory");
                        to[8] = uint8_t(r0); // after XACC's 5 bytes (the 16 bytes from `to` are its and these)
                        to[9] = uint8_t(r1);
                        to[5] = to[6] = to[7] = 0; // not XACC's
                        for (int i = 10; i < 16; ++i) to[i] = 0;
                };
                store(o);
                // CFG too: not in FreeRTOS's list, so the core's, set at boot (Simd::setUp()) and never
                // changed. It must read 3 (misaligned loads and stores) before and after the switches.
                uint32_t cfgNow;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfgNow));
                o[Bytes - 1] = uint8_t(cfgNow & 3u);
                if ((cfgNow & 3u) != 3u) return int32_t(Bytes); // off before we began
                // Switched away from, mid-way: the other task on this core uses PIE in between.
                for (int k = 0; k < 20; ++k) {
                        taskYIELD();
                        for (int spin = 0; spin < 200; ++spin) asm volatile("" ::: "memory");
                }
                store(after);
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfgNow));
                after[Bytes - 1] = uint8_t(cfgNow & 3u);
                for (size_t i = 0; i < Bytes; ++i)
                        if (before[i] != after[i]) return int32_t(i);
#else
                (void)w;
#endif
                return -1;
        }

        // One random case of @p kind, both ways. @return true if the same.
        bool stressCase(StressWorker &w, unsigned kind, uint32_t &detail) {
                Noise       &noise = w.noise;
                uint8_t     *in = w.buf, *a = w.buf + StressBytes / 3, *b = w.buf + 2 * StressBytes / 3;
                const size_t third = StressBytes / 3;
                switch (kind) {
                        case 0: {
                                const int32_t at = stressRegisters(w);
                                detail = uint32_t(at);
                                return at < 0;
                        }
                        case 1: {
                                const size_t n = noise.below(1300), io = noise.below(16), oo = noise.below(16);
                                noise.fill(in, third);
                                std::memset(a, 0x5a, third);
                                std::memset(b, 0x5a, third);
                                Pixels::swap24(in + io, a + oo, n, Simd::Kernel::Scalar);
                                Pixels::swap24(in + io, b + oo, n, Simd::Kernel::Pie);
                                detail = uint32_t(n);
                                return std::memcmp(a, b, third) == 0;
                        }
                        case 2: {
                                const size_t n = noise.below(1000);
                                noise.fill(in, third);
                                std::memset(a, 0x5a, third);
                                std::memset(b, 0x5a, third);
                                const auto *src = reinterpret_cast<const uint16_t *>(in);
                                Pixels::repeat16(src, n, reinterpret_cast<uint16_t *>(a), 2, Simd::Kernel::Scalar);
                                Pixels::repeat16(src, n, reinterpret_cast<uint16_t *>(b), 2, Simd::Kernel::Pie);
                                detail = uint32_t(n);
                                return std::memcmp(a, b, third) == 0;
                        }
                        case 3: {
                                // prev and cur (with 16 zeros before each), then the two filtered rows.
                                const size_t bytes = 3 * (1 + noise.below(600));
                                uint8_t     *prev = in + 16, *cur = prev + 1800 + 16;
                                std::memset(in, 0, 16);
                                std::memset(prev + 1800, 0, 16);
                                pngRows(noise, prev, cur, bytes);
                                PngFilter::row(cur, prev, bytes, a, Simd::Kernel::Scalar);
                                PngFilter::row(cur, prev, bytes, b, Simd::Kernel::Pie);
                                detail = uint32_t(bytes);
                                return std::memcmp(a, b, bytes + 1) == 0;
                        }
                        default: {
                                const size_t      n = noise.below(900), io = noise.below(16), oo = noise.below(16);
                                const uint32_t    bg = noise.next() & 0xffffff;
                                const PixelFormat f = noise.below(2) == 0 ? PixelFormat::Bgr24 : PixelFormat::Rgb24;
                                auto             *pixels = reinterpret_cast<uint32_t *>(in); // 1024 of them
                                iconPixels(noise, pixels, 1024);
                                std::memset(a, 0x5a, third);
                                std::memset(b, 0x5a, third);
                                Pixels::fromArgb(pixels + io, n, a + oo, f, bg, Simd::Kernel::Scalar);
                                Pixels::fromArgb(pixels + io, n, b + oo, f, bg, Simd::Kernel::Pie);
                                detail = uint32_t(n);
                                return std::memcmp(a, b, third) == 0;
                        }
                }
        }

        void stressEntry(void *arg) {
                auto &w = *static_cast<StressWorker *>(arg);
                while (!w.stop->load(std::memory_order_relaxed)) {
                        const unsigned kind = w.noise.below(StressKinds);
                        uint32_t       detail = 0;
                        const bool     same = stressCase(w, kind, detail);
                        ++w.cases[kind];
                        if (!same) {
                                if (w.failures[kind]++ == 0 && w.firstKind == StressKinds) {
                                        w.firstKind = kind;
                                        w.firstCase = w.cases[kind];
                                        w.firstDetail = detail;
                                }
                        }
                }
                w.done.store(true, std::memory_order_release);
                for (;;) Task::sleep(WaitForever); // stopped by runStress()
        }

        int runStress(CommandContext &ctx, StringView arg) {
                unsigned seconds = 10;
                if (!arg.isEmpty()) {
                        seconds = 0;
                        for (char c : arg) {
                                if (c < '0' || c > '9' || seconds > 100000) return ctx.usage();
                                seconds = seconds * 10 + unsigned(c - '0');
                        }
                        if (seconds == 0) return ctx.usage();
                }
                std::atomic<bool> stop{false};
                StressWorker     *workers = static_cast<StressWorker *>(
                        Memory::alloc(MemSpace::Internal, sizeof(StressWorker) * StressWorkers));
                if (workers == nullptr) {
                        ctx.eprintf("simd: out of memory\n");
                        return Shell::ExitFailure;
                }
                unsigned started = 0;
                bool     failed = false;
                for (unsigned i = 0; i < StressWorkers; ++i) {
                        StressWorker *w = ::new (&workers[i]) StressWorker();
                        w->id = i;
                        w->stop = &stop;
                        w->noise.s = 0x9e3779b9u * (i + 1);
                        w->buf = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Internal, StressBytes, 16));
                        Task::Config tc;
                        tc.name = "simdstress";
                        tc.stackBytes = 4096;
                        tc.priority = 1;
                        tc.core = int(i % 2);
                        if (w->buf == nullptr || w->task.start(tc, stressEntry, w).isError()) {
                                failed = true;
                                break;
                        }
                        ++started;
                }
                if (!failed) {
                        ctx.printf("stress: %u tasks (two a core) for %u s\n", StressWorkers, seconds);
                        const uint64_t until = System::uptimeUs() + uint64_t(seconds) * 1000000u;
                        while (System::uptimeUs() < until && !ctx.isInterrupted()) Task::sleep(100);
                }
                stop.store(true, std::memory_order_relaxed);
                for (unsigned i = 0; i < started; ++i)
                        while (!workers[i].done.load(std::memory_order_acquire)) Task::sleep(10);
                int status = failed ? Shell::ExitFailure : Shell::ExitSuccess;
                if (failed) ctx.eprintf("simd: couldn't start the stress tasks\n");
                for (unsigned k = 0; k < StressKinds && !failed; ++k) {
                        uint32_t cases = 0, failures = 0;
                        for (unsigned i = 0; i < started; ++i) {
                                cases += workers[i].cases[k];
                                failures += workers[i].failures[k];
                        }
                        ctx.printf("stress: %s: %lu cases, %lu differ\n", StressNames[k], (unsigned long)cases,
                                   (unsigned long)failures);
                        if (failures != 0) status = Shell::ExitFailure;
                }
                for (unsigned i = 0; i < started; ++i) {
                        const StressWorker &w = workers[i];
                        if (w.firstKind < StressKinds)
                                ctx.printf("stress: task %u (core %u): first %s difference at case %lu (detail %lu)\n",
                                           w.id, w.id % 2, StressNames[w.firstKind], (unsigned long)w.firstCase,
                                           (unsigned long)w.firstDetail);
                }
                for (unsigned i = 0; i < StressWorkers; ++i) {
                        if (i < started) (void)workers[i].task.stop();
                        Memory::free(workers[i].buf);
                        workers[i].~StressWorker();
                }
                Memory::free(workers);
                return status;
        }

        // The kernels: a name and a test (random cases, both ways compared). Their timings are
        // benchmarks (`bench run simd`, below).
        struct Kernel {
                        const char *name;
                        int (*test)(CommandContext &ctx, Noise &noise);
        };

        const Kernel Kernels[] = {
                {"scale",
                 [](CommandContext &ctx, Noise &noise) {
                         return testScale(ctx, noise, 200);
                 }},
                {"glyphs",
                 [](CommandContext &ctx, Noise &) {
                         return testGlyphs(ctx, 500);
                 }},
                {"swap24", testSwap24},
                {"pngfilter", testPngFilter},
                {"softjpeg", testSoftJpeg},
                {"blend", testBlend},
                {"text", testText},
                {"repeat16", testRepeat16},
                {"repeat8", testRepeat8},
        };

        int runSimd(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (!Simd::hasPie()) {
                        ctx.eprintf("simd: this build has no PIE kernels\n");
                        return Shell::ExitFailure;
                }
                if (args.size() < 2 || args.size() > 3) return ctx.usage();
                const StringView what(args[1]);
                const StringView which = args.size() == 3 ? StringView(args[2]) : StringView();
                if (what == "stress") return runStress(ctx, which);
                if (what != "test") return ctx.usage();
                int  status = Shell::ExitSuccess;
                bool found = which.isEmpty();
                for (const Kernel &k : Kernels) {
                        if (!which.isEmpty() && which != k.name) continue;
                        found = true;
                        Noise noise;
                        if (k.test(ctx, noise) != Shell::ExitSuccess) status = Shell::ExitFailure;
                }
                if (!found) {
                        ctx.eprintf("simd: no kernel '%.*s'\n", int(which.size()), which.data());
                        return Shell::ExitFailure;
                }
                return status;
        }

        // ---- The benchmarks (devplan/bench.md; were `simd bench`): each kernel, scalar and PIE.

        Error benchScale(BenchContext &ctx) {
                ImageBuffer in, out;
                in.width = 1920;
                in.height = 1080;
                in.stride = 1920 * 3;
                out.width = 1280;
                out.height = 720;
                out.stride = 1280 * 3;
                Block src(MemSpace::Psram, in.stride * in.height), dst(MemSpace::Psram, out.stride * out.height);
                if (src.p == nullptr || dst.p == nullptr) return Error::NoMem;
                Noise noise;
                noise.fill(src.p, in.stride * in.height);
                in.pixels = src.p;
                out.pixels = dst.p;
                return scalarAndPie(ctx, "", "ms", 1,
                                    [&](Simd::Kernel k) { (void)ImageScaler::scale(in, out, MemSpace::Psram, k); });
        }

        Error benchGlyphs(BenchContext &ctx) {
                const size_t stride = size_t(GlyphStripWidth) * 3;
                Block        a(MemSpace::Internal, stride * Fonts::unscii16.height());
                if (a.p == nullptr) return Error::NoMem;
                Noise noise;
                return scalarAndPie(ctx, "", "us", 100, [&](Simd::Kernel k) {
                        GlyphRenderer r(k == Simd::Kernel::Scalar ? GlyphRenderer::Kernel::Scalar
                                                                  : GlyphRenderer::Kernel::Pie);
                        for (int i = 0; i < 100; ++i) drawCells(r, noise, a.p, stride);
                });
        }

        Error benchSwap24(BenchContext &ctx) {
                const size_t n = 1920 * 1080;
                Block        in(MemSpace::Psram, 3 * n), out(MemSpace::Psram, 3 * n);
                if (in.p == nullptr || out.p == nullptr) return Error::NoMem;
                return scalarAndPie(ctx, "", "ms", 1, [&](Simd::Kernel k) { Pixels::swap24(in.p, out.p, n, k); });
        }

        // As the painter draws icons: 48 × 48 into a strip in internal RAM, from PSRAM (where the
        // icon themes are) and from internal RAM (the kernel's own speed); time an icon.
        // Text's blend: rows of a line's glyph coverage (a third of the pixels covered, some partly)
        // in internal RAM, as the Painter's strip is.
        Error benchText(BenchContext &ctx) {
                constexpr size_t Width = 1024, Rows = 16;
                Block            alpha(MemSpace::Internal, Width), out(MemSpace::Internal, 3 * Width * Rows);
                if (alpha.p == nullptr || out.p == nullptr) return Error::NoMem;
                Noise noise;
                for (size_t i = 0; i < Width; ++i) {
                        const uint32_t k = noise.below(6);
                        alpha.p[i] = k < 4 ? 0 : k == 4 ? 255 : uint8_t(noise.next() >> 24);
                }
                noise.fill(out.p, 3 * Width * Rows);
                for (const TextBlend::Format f : {TextBlend::Format::Bgr24, TextBlend::Format::Gray8}) {
                        const Error err = scalarAndPie(
                                ctx, f == TextBlend::Format::Bgr24 ? "bgr24_" : "gray8_", "us", 1, [&](Simd::Kernel k) {
                                        for (size_t r = 0; r < Rows; ++r)
                                                TextBlend::blend(out.p + r * 3 * Width, f, alpha.p, unsigned(Width),
                                                                 0x203040, k);
                                });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        Error benchBlend(BenchContext &ctx) {
                constexpr size_t Side = 48, Icons = 64;
                Block in(MemSpace::Psram, 4 * Side * Side * Icons), near(MemSpace::Internal, 4 * Side * Side),
                        out(MemSpace::Internal, 3 * Side * Side);
                if (in.p == nullptr || near.p == nullptr || out.p == nullptr) return Error::NoMem;
                Noise noise;
                iconPixels(noise, reinterpret_cast<uint32_t *>(in.p), Side * Side * Icons);
                std::memcpy(near.p, in.p, 4 * Side * Side);
                for (const bool psram : {true, false}) {
                        const Error err =
                                scalarAndPie(ctx, psram ? "psram_" : "internal_", "us", Icons, [&](Simd::Kernel k) {
                                        for (size_t i = 0; i < Icons; ++i) {
                                                const auto *icon = reinterpret_cast<const uint32_t *>(
                                                        psram ? in.p + 4 * Side * Side * i : near.p);
                                                for (size_t r = 0; r < Side; ++r)
                                                        Pixels::fromArgb(icon + r * Side, Side, out.p + r * Side * 3,
                                                                         PixelFormat::Bgr24, 0xc0c0c0, k);
                                        }
                                });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // A 1920-pixel row filtered (the encoder's choice of filter), time a row.
        Error benchPngFilter(BenchContext &ctx) {
                constexpr size_t Bytes = 1920 * 3;
                Block            buf(MemSpace::Internal, 2 * (Bytes + 16) + Bytes + 1);
                if (buf.p == nullptr) return Error::NoMem;
                uint8_t *prev = buf.p + 16, *cur = prev + Bytes + 16, *out = cur + Bytes;
                std::memset(buf.p, 0, 2 * (Bytes + 16));
                Noise noise;
                pngRows(noise, prev, cur, Bytes);
                cur[7] ^= 1; // not equal to the last row (that's quick both ways)
                return scalarAndPie(ctx, "", "us", 100, [&](Simd::Kernel k) {
                        for (int r = 0; r < 100; ++r) PngFilter::row(cur, prev, Bytes, out, k);
                });
        }

        // A 1920 × 1080 4:2:0 JPEG decoded whole and at a quarter.
        Error benchSoftJpeg(BenchContext &ctx) {
                Bytes jpeg;
                jpeg.room = 2 * 1024 * 1024;
                Block store(MemSpace::Psram, jpeg.room), pic(MemSpace::Psram, 1920 * 1080 * 3);
                jpeg.p = store.p;
                Noise noise;
                if (jpeg.p == nullptr || pic.p == nullptr) return Error::NoMem;
                if (!makeJpeg(noise, 1920, 1080, JpegHeader::Sampling::Yuv420, jpeg)) return Error::NoMem;
                for (unsigned shift : {0u, 2u}) {
                        ImageBuffer out;
                        out.width = 1920 >> shift;
                        out.height = 1080 >> shift;
                        out.stride = size_t(out.width) * 3;
                        out.pixels = pic.p;
                        const Error err =
                                scalarAndPie(ctx, shift == 0 ? "full_" : "quarter_", "ms", 1, [&](Simd::Kernel k) {
                                        (void)SoftJpeg::decode(ByteSpan(jpeg.p, jpeg.n), out, shift, MemSpace::Internal,
                                                               k);
                                });
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // Orienting (no vector kernel): a 1920 × 1080 picture turned a quarter right.
        Error benchOrient(BenchContext &ctx) {
                ImageBuffer in, out;
                in.width = out.height = 1920;
                in.height = out.width = 1080;
                in.stride = 1920 * 3;
                out.stride = 1080 * 3;
                Block a(MemSpace::Psram, in.stride * in.height), b(MemSpace::Psram, out.stride * out.height);
                if (a.p == nullptr || b.p == nullptr) return Error::NoMem;
                in.pixels = a.p;
                out.pixels = b.p;
                return ctx.measure("quarter_right_ms", "ms", BenchBetter::Lower, [&](double &ms) {
                        Stopwatch watch;
                        (void)ImageScaler::orient(in, out, 6);
                        ms = double(watch.elapsedUs()) / 1000;
                        return Error(Error::Ok);
                });
        }

        // repeat16 at 2: 360 rows of 640 (a scaled window's 565 rows).
        Error benchRepeat16(BenchContext &ctx) {
                Block in(MemSpace::Internal, 640 * 2), out(MemSpace::Internal, 1280 * 2);
                if (in.p == nullptr || out.p == nullptr) return Error::NoMem;
                return scalarAndPie(ctx, "", "us", 1, [&](Simd::Kernel k) {
                        for (int i = 0; i < 360; ++i)
                                Pixels::repeat16(reinterpret_cast<const uint16_t *>(in.p), 640,
                                                 reinterpret_cast<uint16_t *>(out.p), 2, k);
                });
        }

} // namespace

BMDS_BENCH(simdScale, "simd.scale", benchScale, BenchQuick, "The scaler: 1920x1080 to 1280x720 BGR24 in PSRAM")
BMDS_BENCH(simdGlyphs, "simd.glyphs", benchGlyphs, BenchQuick, "Glyphs: 80 cells of 8x16 into internal RAM")
BMDS_BENCH(simdSwap24, "simd.swap24", benchSwap24, BenchQuick, "BGR to RGB: 1920x1080 in PSRAM")
BMDS_BENCH(simdBlend, "simd.blend", benchBlend, BenchQuick,
           "Icons' alpha: a 48x48 icon from PSRAM and from internal RAM")
BMDS_BENCH(simdText, "simd.text", benchText, BenchQuick,
           "Text's blend: a color by alpha over 16 rows of 1024 pixels, B, G, R and grey, internal RAM (us)")
BMDS_BENCH(simdPngFilter, "simd.pngfilter", benchPngFilter, BenchQuick, "PNG's filters: a 1920-pixel row")
BMDS_BENCH(simdSoftJpeg, "simd.softjpeg", benchSoftJpeg, BenchQuick,
           "The JPEG decoder: 1920x1080 4:2:0, whole and at a quarter")
BMDS_BENCH(simdOrient, "simd.orient", benchOrient, BenchQuick, "Orienting: 1920x1080 BGR24 turned a quarter right")
BMDS_BENCH(simdRepeat16, "simd.repeat16", benchRepeat16, BenchQuick, "565 pixels doubled: 360 rows of 640")

BMDS_SHELL_COMMAND(simd, runSimd, "test [KERNEL] | stress [SECONDS]",
                   "Check the PIE vector kernels byte for byte against their scalar twins, or from tasks switching "
                   "on both cores (PIE's registers kept across switches); their speed: 'bench run simd'")

BMDS_NAMESPACE_END
