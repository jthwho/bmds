/**
 * @file      imagescaler.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pictures resized, turned and mirrored (devplan/images.md §3e). Each axis has a table of taps:
 * for every output pixel, the input pixels it takes and their weights (summing to One). Down,
 * the input it covers, each by how much; up, the two nearest, bilinear. Rows are filtered across
 * Each output row is its input rows summed (down: a vector's work, PIE's), then filtered across.
 */

#include <cstddef>
#include <cstring>
#include <bmds/imagescaler.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t One = 4096; // a weight of 1, 12 bits

        // An axis's taps: output i takes `count[i]` inputs from `first[i]`, weights from `at[i]`.
        struct Taps {
                        uint32_t *first = nullptr;
                        uint16_t *count = nullptr;
                        uint32_t *at = nullptr;
                        uint16_t *weight = nullptr;
                        uint32_t  most = 0; // the most taps an output takes
        };

        // The taps' sizes for @p from inputs to @p to outputs: how many weights in all.
        size_t weightCount(uint32_t from, uint32_t to) {
                return to >= from ? size_t(to) * 2
                                  : size_t(from) + to; // up: 2 each; down: each input touches 2 at most
        }

        void makeTaps(uint32_t from, uint32_t to, Taps &t) {
                size_t   w = 0;
                uint32_t most = 0;
                for (uint32_t i = 0; i < to; ++i) {
                        t.first[i] = 0;
                        t.at[i] = uint32_t(w);
                        uint32_t sum = 0;
                        uint16_t n = 0;
                        if (to >= from) { // bilinear: centre (i + 0.5) × from / to − 0.5, in 1/One
                                int64_t centre = ((int64_t(2 * i + 1) * from * One) / (2 * int64_t(to))) - One / 2;
                                if (centre < 0) centre = 0;
                                uint32_t j = uint32_t(centre / One);
                                uint32_t frac = uint32_t(centre % One);
                                if (j >= from - 1) {
                                        j = from - 1;
                                        frac = 0;
                                }
                                t.first[i] = j;
                                t.weight[w++] = uint16_t(One - frac);
                                n = 1;
                                if (frac != 0) {
                                        t.weight[w++] = uint16_t(frac);
                                        n = 2;
                                }
                        } else { // the input [i, i+1) × from / to covers, in 1/One of an input pixel
                                const uint64_t p0 = uint64_t(i) * from * One / to;
                                const uint64_t p1 = uint64_t(i + 1) * from * One / to;
                                const uint64_t span = p1 - p0;
                                const uint32_t j0 = uint32_t(p0 / One);
                                t.first[i] = j0;
                                for (uint32_t j = j0; uint64_t(j) * One < p1 && j < from; ++j) {
                                        const uint64_t a = uint64_t(j) * One > p0 ? uint64_t(j) * One : p0;
                                        const uint64_t b = uint64_t(j + 1) * One < p1 ? uint64_t(j + 1) * One : p1;
                                        const uint32_t v = uint32_t((b - a) * One / span);
                                        t.weight[w++] = uint16_t(v);
                                        sum += v;
                                        ++n;
                                }
                                t.weight[w - 1] = uint16_t(t.weight[w - 1] + (One - sum)); // rounding: exactly One
                        }
                        t.count[i] = n;
                        if (n > most) most = n;
                }
                t.most = most;
                return;
        }

        constexpr unsigned DownShift = 8;     // down: value × One >> 8 = value × 16 (12 bits) in a uint16
        constexpr unsigned AcrossShift2 = 16; // across: (value × 16) × One >> 16 = value

        // Down, scalar: @p n values of an output row = Σ row k (@p row0 + k × @p stride) × w[k], rounded,
        // >> DownShift.
        void downScalar(const uint8_t *row0, size_t stride, const uint16_t *w, uint32_t taps, uint16_t *mid,
                        size_t from, size_t n) {
                for (size_t i = from; i < n; ++i) {
                        uint32_t       sum = 1u << (DownShift - 1);
                        const uint8_t *p = row0 + i;
                        for (uint32_t k = 0; k < taps; ++k, p += stride) sum += uint32_t(*p) * w[k];
                        mid[i] = uint16_t(sum >> DownShift);
                }
                return;
        }

#if BMDS_HAS_PIE
        // Down, PIE: 16 values a step, 8 at a time in QACC: a hardware loop over the taps (the 16 bytes
        // of each row widened to u16, × the tap's weight broadcast, accumulated in 64 bits), the rounding
        // added first as 128 × 1, shifted out (saturated to u16: never reached). The rest scalar.
        // Expects CFG's misaligned loads on and q6 all 128, q7 all ones; `mid` 16-byte aligned.
        void downPie(const uint8_t *row0, size_t stride, const uint16_t *w, uint32_t taps, uint16_t *mid, size_t n) {
                const size_t   whole = n & ~size_t(15);
                const uint32_t sh = DownShift;
                for (size_t i = 0; i < whole; i += 16) {
                        const uint8_t  *s0 = row0 + i, *s1 = row0 + i;
                        const uint16_t *w0 = w, *w1 = w;
                        uint16_t       *d = mid + i;
                        uint32_t        c0 = taps, c1 = taps;
                        asm volatile("esp.zero.qacc\n"
                                     "esp.vmulas.u16.qacc q6, q7\n"
                                     "1: esp.vldext.u8.xp q0, q1, %[s0], %[st]\n"
                                     "esp.vldbc.16.ip q2, %[w0], 2\n"
                                     "addi %[c0], %[c0], -1\n"
                                     "esp.vmulas.u16.qacc q0, q2\n"
                                     "bnez %[c0], 1b\n"
                                     "esp.srcmb.u16.qacc q3, %[sh], 1\n"
                                     "esp.vst.128.ip q3, %[d], 16\n"
                                     "esp.zero.qacc\n"
                                     "esp.vmulas.u16.qacc q6, q7\n"
                                     "2: esp.vldext.u8.xp q0, q1, %[s1], %[st]\n"
                                     "esp.vldbc.16.ip q2, %[w1], 2\n"
                                     "addi %[c1], %[c1], -1\n"
                                     "esp.vmulas.u16.qacc q1, q2\n"
                                     "bnez %[c1], 2b\n"
                                     "esp.srcmb.u16.qacc q3, %[sh], 1\n"
                                     "esp.vst.128.ip q3, %[d], 16\n"
                                     : [s0] "+cr"(s0), [s1] "+cr"(s1), [w0] "+cr"(w0), [w1] "+cr"(w1), [d] "+cr"(d),
                                       [c0] "+r"(c0), [c1] "+r"(c1)
                                     : [st] "cr"(stride), [sh] "cr"(sh)
                                     : "memory");
                }
                downScalar(row0, stride, w, taps, mid, whole, n);
                return;
        }
#endif

        // Across, scalar: a row of `mid` (value × 16 a channel) to 8 bits, @p Ch channels a pixel.
        template <unsigned Ch> void acrossFixed(const uint16_t *mid, const Taps &t, uint32_t width, uint8_t *out) {
                constexpr uint32_t Round = 1u << (AcrossShift2 - 1);
                for (uint32_t x = 0; x < width; ++x, out += Ch) {
                        const uint16_t *w = t.weight + t.at[x];
                        const uint16_t *s = mid + size_t(t.first[x]) * Ch;
                        const uint32_t  n = t.count[x];
                        uint32_t        sum[Ch];
                        for (unsigned c = 0; c < Ch; ++c) sum[c] = Round;
                        for (uint32_t k = 0; k < n; ++k, s += Ch) {
                                const uint32_t wk = w[k];
                                for (unsigned c = 0; c < Ch; ++c) sum[c] += s[c] * wk;
                        }
                        for (unsigned c = 0; c < Ch; ++c) {
                                const uint32_t v = sum[c] >> AcrossShift2;
                                out[c] = uint8_t(v > 255 ? 255 : v);
                        }
                }
                return;
        }

#if BMDS_HAS_PIE
        // Across, PIE: a pixel at a time, its channels in lanes 0 to Ch-1 (an unaligned load at each tap,
        // × the tap's weight broadcast, accumulated in QACC; the rounding added as 0x8000 × 1; shifted
        // out by 16). The lanes past Ch are ignored. Reads up to 16 bytes past the row (padded). Expects
        // CFG's misaligned loads on and q7 all ones, q5 all 0x8000.
        template <unsigned Ch> void acrossPie(const uint16_t *mid, const Taps &t, uint32_t width, uint8_t *out) {
                const uint32_t sh = AcrossShift2;
                const uint32_t step = Ch * 2;
                for (uint32_t x = 0; x < width; ++x, out += Ch) {
                        const uint16_t *w = t.weight + t.at[x];
                        const uint16_t *src = mid + size_t(t.first[x]) * Ch;
                        uint32_t        v0, v1 = 0, v2 = 0, v3 = 0;
                        uint32_t        n = t.count[x];
                        asm volatile("esp.zero.qacc\n"
                                     "esp.vmulas.u16.qacc q5, q7\n"
                                     "1: esp.vld.128.xp q0, %[s], %[step]\n"
                                     "esp.vldbc.16.ip q2, %[w], 2\n"
                                     "addi %[n], %[n], -1\n"
                                     "esp.vmulas.u16.qacc q0, q2\n"
                                     "bnez %[n], 1b\n"
                                     "esp.srcmb.u16.qacc q3, %[sh], 1\n"
                                     "esp.movi.16.a q3, %[v0], 0\n"
                                     : [s] "+cr"(src), [w] "+cr"(w), [v0] "=&cr"(v0), [n] "+r"(n)
                                     : [step] "cr"(step), [sh] "cr"(sh)
                                     : "memory");
                        if constexpr (Ch >= 3) {
                                asm volatile("esp.movi.16.a q3, %0, 1\n"
                                             "esp.movi.16.a q3, %1, 2\n"
                                             : "=cr"(v1), "=cr"(v2));
                        }
                        if constexpr (Ch == 4) asm volatile("esp.movi.16.a q3, %0, 3\n" : "=cr"(v3));
                        out[0] = uint8_t(v0 > 255 ? 255 : v0);
                        if constexpr (Ch >= 3) {
                                out[1] = uint8_t(v1 > 255 ? 255 : v1);
                                out[2] = uint8_t(v2 > 255 ? 255 : v2);
                        }
                        if constexpr (Ch == 4) out[3] = uint8_t(v3 > 255 ? 255 : v3);
                }
                return;
        }

        void downPieSetUp(const uint8_t *row0, size_t stride, const uint16_t *w, uint32_t taps, uint16_t *mid,
                          size_t n) {
                static const uint16_t Round[8] __attribute__((aligned(16))) = {128, 128, 128, 128, 128, 128, 128, 128};
                static const uint16_t Ones[8] __attribute__((aligned(16))) = {1, 1, 1, 1, 1, 1, 1, 1};
                const uint16_t       *rp = Round, *op = Ones;
                uint32_t              cfg;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfg));
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg | 2u)); // misaligned loads by the hardware
                asm volatile("esp.vld.128.ip q6, %0, 0\n"
                             "esp.vld.128.ip q7, %1, 0\n" ::"cr"(rp),
                             "cr"(op)
                             : "memory");
                downPie(row0, stride, w, taps, mid, n);
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg));
                return;
        }

        void acrossRowPie(const uint16_t *mid, const Taps &t, uint32_t width, unsigned ch, uint8_t *out) {
                static const uint16_t Half[8]
                        __attribute__((aligned(16))) = {0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000};
                static const uint16_t Ones[8] __attribute__((aligned(16))) = {1, 1, 1, 1, 1, 1, 1, 1};
                const uint16_t       *hp = Half, *op = Ones;
                uint32_t              cfg;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfg));
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg | 2u));
                asm volatile("esp.vld.128.ip q5, %0, 0\n"
                             "esp.vld.128.ip q7, %1, 0\n" ::"cr"(hp),
                             "cr"(op)
                             : "memory");
                if (ch == 3)
                        acrossPie<3>(mid, t, width, out);
                else if (ch == 4)
                        acrossPie<4>(mid, t, width, out);
                else
                        acrossPie<1>(mid, t, width, out);
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg));
                return;
        }
#endif

        void acrossRow(const uint16_t *mid, const Taps &t, uint32_t width, unsigned ch, uint8_t *out) {
                if (ch == 3)
                        acrossFixed<3>(mid, t, width, out);
                else if (ch == 4)
                        acrossFixed<4>(mid, t, width, out);
                else
                        acrossFixed<1>(mid, t, width, out);
                return;
        }

        unsigned channelsOf(PixelFormat f) {
                return f == PixelFormat::Gray8 ? 1 : f == PixelFormat::Bgra32 ? 4 : 3;
        }

} // namespace

ImageScaler::Size ImageScaler::fit(uint32_t width, uint32_t height, uint32_t boxWidth, uint32_t boxHeight,
                                   bool enlarge) {
        if (width == 0 || height == 0 || boxWidth == 0 || boxHeight == 0) return Size{};
        if (!enlarge && width <= boxWidth && height <= boxHeight) return Size{width, height};
        // The side that limits: box width / width against box height / height.
        Size s;
        if (uint64_t(boxWidth) * height <= uint64_t(boxHeight) * width) {
                s.width = boxWidth;
                s.height = uint32_t((uint64_t(height) * boxWidth + width / 2) / width);
        } else {
                s.height = boxHeight;
                s.width = uint32_t((uint64_t(width) * boxHeight + height / 2) / height);
        }
        if (s.width == 0) s.width = 1;
        if (s.height == 0) s.height = 1;
        return s;
}

Error ImageScaler::scale(const ImageBuffer &in, const ImageBuffer &out, MemSpace space, Simd::Kernel kernel) {
        if (!in.isValid() || !out.isValid() || in.format != out.format) return Error::Invalid;
        if (in.format == PixelFormat::Rgb565) return Error::NotSupported;
        const unsigned ch = channelsOf(in.format);
        const unsigned bpp = bytesPerPixel(in.format);
        if (in.width == out.width && in.height == out.height) {
                for (uint32_t y = 0; y < in.height; ++y) std::memcpy(out.row(y), in.row(y), size_t(in.width) * bpp);
                return Error();
        }
        // Down first (whole input rows, a vector's work), then across (only the output's rows). The taps
        // and the row pointers in @p space; the middle row (16-byte aligned) in internal RAM if there's room.
        const size_t wx = weightCount(in.width, out.width), wy = weightCount(in.height, out.height);
        const size_t tapBytes = (size_t(out.width) + out.height) * (4 + 2 + 4) + (wx + wy) * 2 + 64;
        const size_t midValues = size_t(in.width) * ch;
        const size_t midBytes = ((midValues * 2 + 15) & ~size_t(15)) + 16; // + 16: the vector loads read past
        auto        *mem = static_cast<uint8_t *>(Memory::alloc(space, tapBytes));
        void        *midMem = Memory::allocAligned(MemSpace::Internal, midBytes, 16);
        if (midMem == nullptr) midMem = Memory::allocAligned(space, midBytes, 16);
        if (mem == nullptr || midMem == nullptr) {
                Memory::free(mem);
                Memory::free(midMem);
                return Error::NoMem;
        }
        uint8_t   *p = mem;
        const auto take = [&p](size_t bytes, size_t align) {
                p = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(p) + align - 1) & ~uintptr_t(align - 1));
                uint8_t *r = p;
                p += bytes;
                return r;
        };
        Taps tx, ty;
        tx.first = reinterpret_cast<uint32_t *>(take(out.width * 4u, 4));
        tx.at = reinterpret_cast<uint32_t *>(take(out.width * 4u, 4));
        ty.first = reinterpret_cast<uint32_t *>(take(out.height * 4u, 4));
        ty.at = reinterpret_cast<uint32_t *>(take(out.height * 4u, 4));
        tx.count = reinterpret_cast<uint16_t *>(take(out.width * 2u, 2));
        ty.count = reinterpret_cast<uint16_t *>(take(out.height * 2u, 2));
        tx.weight = reinterpret_cast<uint16_t *>(take(wx * 2, 2));
        ty.weight = reinterpret_cast<uint16_t *>(take(wy * 2, 2));
        auto *mid = static_cast<uint16_t *>(midMem);
        makeTaps(in.width, out.width, tx);
        makeTaps(in.height, out.height, ty);
        const Simd::Kernel k = Simd::usable(kernel);
        for (uint32_t y = 0; y < out.height; ++y) {
                const uint32_t  taps = ty.count[y];
                const uint8_t  *row0 = in.row(ty.first[y]);
                const uint16_t *w = ty.weight + ty.at[y];
#if BMDS_HAS_PIE
                if (k == Simd::Kernel::Pie)
                        downPieSetUp(row0, in.stride, w, taps, mid, midValues);
                else
#endif
                        downScalar(row0, in.stride, w, taps, mid, 0, midValues);
#if BMDS_HAS_PIE
                if (k == Simd::Kernel::Pie)
                        acrossRowPie(mid, tx, out.width, ch, out.row(y));
                else
#endif
                        acrossRow(mid, tx, out.width, ch, out.row(y));
        }
        (void)k;
        Memory::free(midMem);
        Memory::free(mem);
        return Error();
}

ImageScaler::Size ImageScaler::oriented(uint32_t width, uint32_t height, uint8_t orientation) {
        return orientation >= 5 && orientation <= 8 ? Size{height, width} : Size{width, height};
}

namespace {

        // Copies a tile's rows: output pixel x of row y from @p src + y × dy + x × dx (bytes; either may
        // be negative), @p Bpp bytes a pixel.
        template <unsigned Bpp>
        void orientTile(const uint8_t *src, ptrdiff_t dx, ptrdiff_t dy, uint8_t *dst, size_t dstStride, uint32_t w,
                        uint32_t h) {
                for (uint32_t y = 0; y < h; ++y, src += dy, dst += dstStride) {
                        const uint8_t *p = src;
                        uint8_t       *d = dst;
                        for (uint32_t x = 0; x < w; ++x, p += dx, d += Bpp) {
                                if constexpr (Bpp == 4) {
                                        uint32_t v;
                                        std::memcpy(&v, p, 4);
                                        std::memcpy(d, &v, 4);
                                } else {
                                        for (unsigned c = 0; c < Bpp; ++c) d[c] = p[c];
                                }
                        }
                }
                return;
        }

} // namespace

// Each orientation is a start and two steps in the source: output (x, y) is source start + x × dx + y × dy.
// The output goes in 32 × 32 tiles, so a quarter turn's column of source reads stays in the cache.
Error ImageScaler::orient(const ImageBuffer &in, const ImageBuffer &out, uint8_t orientation) {
        const Size s = oriented(in.width, in.height, orientation);
        if (!in.isValid() || !out.isValid() || in.format != out.format || out.width != s.width ||
            out.height != s.height)
                return Error::Invalid;
        const ptrdiff_t bpp = ptrdiff_t(bytesPerPixel(in.format));
        const ptrdiff_t st = ptrdiff_t(in.stride);
        const uint8_t  *first = in.row(0), *lastRow = in.row(in.height - 1);
        const ptrdiff_t lastCol = ptrdiff_t(in.width - 1) * bpp;
        const uint8_t  *start = first;
        ptrdiff_t       dx = bpp, dy = st;
        switch (orientation) {
                case 2: start = first + lastCol, dx = -bpp, dy = st; break;    // mirrored across
                case 3: start = lastRow + lastCol, dx = -bpp, dy = -st; break; // turned half
                case 4: start = lastRow, dx = bpp, dy = -st; break;            // mirrored down
                case 5: start = first, dx = st, dy = bpp; break;               // transposed
                case 6: start = lastRow, dx = -st, dy = bpp; break;            // turned a quarter right
                case 7: start = lastRow + lastCol, dx = -st, dy = -bpp; break; // transversed
                case 8: start = first + lastCol, dx = st, dy = -bpp; break;    // turned a quarter left
                default: break;
        }
        constexpr uint32_t Tile = 32;
        for (uint32_t ty = 0; ty < out.height; ty += Tile)
                for (uint32_t tx = 0; tx < out.width; tx += Tile) {
                        const uint32_t w = out.width - tx < Tile ? out.width - tx : Tile;
                        const uint32_t h = out.height - ty < Tile ? out.height - ty : Tile;
                        const uint8_t *src = start + ptrdiff_t(ty) * dy + ptrdiff_t(tx) * dx;
                        uint8_t       *dst = out.row(ty) + size_t(tx) * size_t(bpp);
                        switch (bpp) {
                                case 3: orientTile<3>(src, dx, dy, dst, out.stride, w, h); break;
                                case 4: orientTile<4>(src, dx, dy, dst, out.stride, w, h); break;
                                case 2: orientTile<2>(src, dx, dy, dst, out.stride, w, h); break;
                                default: orientTile<1>(src, dx, dy, dst, out.stride, w, h); break;
                        }
                }
        return Error();
}

BMDS_NAMESPACE_END
