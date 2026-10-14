/**
 * @file      simdcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `simd` command (devplan/simd.md §3): each vector kernel timed against its scalar twin, and
 * checked byte for byte against it on random input. Its output format is a userspace interface:
 * keep the lines and their fields, add new ones only at the end.
 */

#include <cstring>
#include <bmds/command.h>
#include <bmds/font.h>
#include <bmds/glyphrenderer.h>
#include <bmds/image.h>
#include <bmds/imagescaler.h>
#include <bmds/images.h>
#include <bmds/memory.h>
#include <bmds/softjpeg.h>
#include <bmds/pngfilter.h>
#include <bmds/shell.h>
#include <bmds/simd.h>
#include <bmds/system.h>

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

        uint32_t msSince(uint64_t startUs) {
                return uint32_t((System::uptimeUs() - startUs) / 1000);
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

        void benchScale(CommandContext &ctx) {
                ImageBuffer in, out;
                in.width = 1920;
                in.height = 1080;
                in.stride = 1920 * 3;
                out.width = 1280;
                out.height = 720;
                out.stride = 1280 * 3;
                Block src(MemSpace::Psram, in.stride * in.height), dst(MemSpace::Psram, out.stride * out.height);
                if (src.p == nullptr || dst.p == nullptr) return;
                Noise noise;
                noise.fill(src.p, in.stride * in.height);
                in.pixels = src.p;
                out.pixels = dst.p;
                uint32_t ms[2];
                for (int k = 0; k < 2; ++k) {
                        const uint64_t t = System::uptimeUs();
                        (void)ImageScaler::scale(in, out, MemSpace::Psram,
                                                 k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                        ms[k] = msSince(t);
                }
                ctx.printf("scale 1920x1080 to 1280x720 bgr24: scalar %u ms, pie %u ms\n", unsigned(ms[0]),
                           unsigned(ms[1]));
                return;
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

        void benchGlyphs(CommandContext &ctx) {
                const size_t  stride = size_t(GlyphStripWidth) * 3;
                Block         a(MemSpace::Internal, stride * Fonts::unscii16.height());
                GlyphRenderer r[2] = {GlyphRenderer(GlyphRenderer::Kernel::Scalar),
                                      GlyphRenderer(GlyphRenderer::Kernel::Pie)};
                if (a.p == nullptr) return;
                uint32_t us[2];
                for (int k = 0; k < 2; ++k) {
                        Noise          noise;
                        const uint64_t t = System::uptimeUs();
                        for (int i = 0; i < 100; ++i) drawCells(r[k], noise, a.p, stride);
                        us[k] = uint32_t(System::uptimeUs() - t) / 100;
                }
                ctx.printf("glyphs 80 cells of 8x16: scalar %u us, pie %u us\n", unsigned(us[0]), unsigned(us[1]));
                return;
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

        void benchSwap24(CommandContext &ctx) {
                const size_t n = 1920 * 1080;
                Block        in(MemSpace::Psram, 3 * n), out(MemSpace::Psram, 3 * n);
                if (in.p == nullptr || out.p == nullptr) return;
                uint32_t ms[2];
                for (int k = 0; k < 2; ++k) {
                        const uint64_t t = System::uptimeUs();
                        Pixels::swap24(in.p, out.p, n, k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                        ms[k] = msSince(t);
                }
                ctx.printf("swap24 1920x1080 psram: scalar %u ms, pie %u ms\n", unsigned(ms[0]), unsigned(ms[1]));
                return;
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

        // As the painter draws icons: 48 × 48 into a strip in internal RAM, from PSRAM (where the
        // icon themes are) and from internal RAM (the kernel's own speed).
        void benchBlend(CommandContext &ctx) {
                constexpr size_t Side = 48, Icons = 64;
                Block in(MemSpace::Psram, 4 * Side * Side * Icons), near(MemSpace::Internal, 4 * Side * Side),
                        out(MemSpace::Internal, 3 * Side * Side);
                if (in.p == nullptr || near.p == nullptr || out.p == nullptr) return;
                Noise noise;
                iconPixels(noise, reinterpret_cast<uint32_t *>(in.p), Side * Side * Icons);
                std::memcpy(near.p, in.p, 4 * Side * Side);
                for (const bool psram : {true, false}) {
                        uint32_t us[2];
                        for (int k = 0; k < 2; ++k) {
                                const uint64_t t = System::uptimeUs();
                                for (size_t i = 0; i < Icons; ++i) {
                                        const auto *icon = reinterpret_cast<const uint32_t *>(
                                                psram ? in.p + 4 * Side * Side * i : near.p);
                                        for (size_t r = 0; r < Side; ++r)
                                                Pixels::fromArgb(icon + r * Side, Side, out.p + r * Side * 3,
                                                                 PixelFormat::Bgr24, 0xc0c0c0,
                                                                 k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                                }
                                us[k] = uint32_t(System::uptimeUs() - t) / Icons;
                        }
                        ctx.printf("blend a 48x48 icon from %s: scalar %u us, pie %u us\n",
                                   psram ? "psram" : "internal", unsigned(us[0]), unsigned(us[1]));
                }
                return;
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

        void benchPngFilter(CommandContext &ctx) {
                constexpr size_t Bytes = 1920 * 3;
                Block            buf(MemSpace::Internal, 2 * (Bytes + 16) + Bytes + 1);
                if (buf.p == nullptr) return;
                uint8_t *prev = buf.p + 16, *cur = prev + Bytes + 16, *out = cur + Bytes;
                std::memset(buf.p, 0, 2 * (Bytes + 16));
                Noise    noise;
                uint32_t us[2];
                pngRows(noise, prev, cur, Bytes);
                cur[7] ^= 1; // not equal to the last row (that's quick both ways)
                for (int k = 0; k < 2; ++k) {
                        const uint64_t t = System::uptimeUs();
                        for (int r = 0; r < 100; ++r)
                                PngFilter::row(cur, prev, Bytes, out,
                                               k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                        us[k] = uint32_t(System::uptimeUs() - t) / 100;
                }
                ctx.printf("pngfilter a 1920-pixel row: scalar %u us, pie %u us\n", unsigned(us[0]), unsigned(us[1]));
                return;
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

        void benchSoftJpeg(CommandContext &ctx) {
                Bytes jpeg;
                jpeg.room = 2 * 1024 * 1024;
                Block store(MemSpace::Psram, jpeg.room), pic(MemSpace::Psram, 1920 * 1080 * 3);
                jpeg.p = store.p;
                Noise noise;
                if (jpeg.p == nullptr || pic.p == nullptr ||
                    !makeJpeg(noise, 1920, 1080, JpegHeader::Sampling::Yuv420, jpeg))
                        return;
                for (unsigned shift : {0u, 2u}) {
                        ImageBuffer out;
                        out.width = 1920 >> shift;
                        out.height = 1080 >> shift;
                        out.stride = size_t(out.width) * 3;
                        out.pixels = pic.p;
                        uint32_t ms[2];
                        for (int k = 0; k < 2; ++k) {
                                const uint64_t t = System::uptimeUs();
                                (void)SoftJpeg::decode(ByteSpan(jpeg.p, jpeg.n), out, shift, MemSpace::Internal,
                                                       k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                                ms[k] = msSince(t);
                        }
                        ctx.printf("softjpeg 1920x1080 4:2:0 at 1/%u: scalar %u ms, pie %u ms\n", 1u << shift,
                                   unsigned(ms[0]), unsigned(ms[1]));
                }
                return;
        }

        // Orienting (not a vector kernel: timed alone): a 1920 × 1080 picture turned a quarter right.
        int testOrient(CommandContext &ctx, Noise &) {
                ctx.printf("orient: no vector kernel (bench only)\n");
                return Shell::ExitSuccess;
        }

        void benchOrient(CommandContext &ctx) {
                ImageBuffer in, out;
                in.width = out.height = 1920;
                in.height = out.width = 1080;
                in.stride = 1920 * 3;
                out.stride = 1080 * 3;
                Block a(MemSpace::Psram, in.stride * in.height), b(MemSpace::Psram, out.stride * out.height);
                if (a.p == nullptr || b.p == nullptr) return;
                in.pixels = a.p;
                out.pixels = b.p;
                const uint64_t t = System::uptimeUs();
                (void)ImageScaler::orient(in, out, 6);
                ctx.printf("orient 1920x1080 bgr24 a quarter right: %u ms\n", unsigned(msSince(t)));
                return;
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

        void benchRepeat16(CommandContext &ctx) {
                Block in(MemSpace::Internal, 640 * 2), out(MemSpace::Internal, 1280 * 2);
                if (in.p == nullptr || out.p == nullptr) return;
                uint32_t us[2];
                for (int k = 0; k < 2; ++k) {
                        const uint64_t t = System::uptimeUs();
                        for (int i = 0; i < 360; ++i)
                                Pixels::repeat16(reinterpret_cast<const uint16_t *>(in.p), 640,
                                                 reinterpret_cast<uint16_t *>(out.p), 2,
                                                 k == 0 ? Simd::Kernel::Scalar : Simd::Kernel::Pie);
                        us[k] = uint32_t(System::uptimeUs() - t);
                }
                ctx.printf("repeat16 360 rows of 640 at 2: scalar %u us, pie %u us\n", unsigned(us[0]),
                           unsigned(us[1]));
                return;
        }

        // The kernels: a name, a test (random cases, both ways compared) and a bench.
        struct Kernel {
                        const char *name;
                        int (*test)(CommandContext &ctx, Noise &noise);
                        void (*bench)(CommandContext &ctx);
        };

        const Kernel Kernels[] = {
                {"scale", [](CommandContext &ctx, Noise &noise) { return testScale(ctx, noise, 200); }, benchScale},
                {"glyphs", [](CommandContext &ctx, Noise &) { return testGlyphs(ctx, 500); }, benchGlyphs},
                {"swap24", testSwap24, benchSwap24},
                {"pngfilter", testPngFilter, benchPngFilter},
                {"softjpeg", testSoftJpeg, benchSoftJpeg},
                {"orient", testOrient, benchOrient},
                {"blend", testBlend, benchBlend},
                {"repeat16", testRepeat16, benchRepeat16},
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
                if (what != "bench" && what != "test") return ctx.usage();
                int  status = Shell::ExitSuccess;
                bool found = which.isEmpty();
                for (const Kernel &k : Kernels) {
                        if (!which.isEmpty() && which != k.name) continue;
                        found = true;
                        if (what == "bench") {
                                k.bench(ctx);
                                continue;
                        }
                        Noise noise;
                        if (k.test(ctx, noise) != Shell::ExitSuccess) status = Shell::ExitFailure;
                }
                if (!found) {
                        ctx.eprintf("simd: no kernel '%.*s'\n", int(which.size()), which.data());
                        return Shell::ExitFailure;
                }
                return status;
        }

} // namespace

BMDS_SHELL_COMMAND(simd, runSimd, "bench [KERNEL] | test [KERNEL]",
                   "Time the PIE vector kernels against their scalar twins, or check them byte for byte")

BMDS_NAMESPACE_END
