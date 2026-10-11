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
 * once each into a ring of as many rows as an output row takes, then down.
 */

#include <cstring>
#include <bmds/imagescaler.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t One = 4096;      // a weight of 1, 12 bits
        constexpr unsigned AcrossShift = 8; // filtered across: value × 16 (12 bits) in a uint16

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

        // A row filtered across into @p r (value × 16 a channel), @p ch channels a pixel. @hotpath
        template <unsigned Ch> void acrossFixed(const uint8_t *src, const Taps &t, uint32_t width, uint16_t *r) {
                constexpr uint32_t Round = 1u << (AcrossShift - 1);
                for (uint32_t x = 0; x < width; ++x, r += Ch) {
                        const uint16_t *w = t.weight + t.at[x];
                        const uint8_t  *s = src + size_t(t.first[x]) * Ch;
                        const uint32_t  n = t.count[x];
                        uint32_t        sum[Ch] = {};
                        for (uint32_t k = 0; k < n; ++k, s += Ch) {
                                const uint32_t wk = w[k];
                                for (unsigned c = 0; c < Ch; ++c) sum[c] += s[c] * wk;
                        }
                        for (unsigned c = 0; c < Ch; ++c) r[c] = uint16_t((sum[c] + Round) >> AcrossShift);
                }
                return;
        }

        void acrossRow(const uint8_t *src, const Taps &t, uint32_t width, unsigned ch, uint16_t *r) {
                if (ch == 3)
                        acrossFixed<3>(src, t, width, r);
                else if (ch == 4)
                        acrossFixed<4>(src, t, width, r);
                else
                        acrossFixed<1>(src, t, width, r);
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

Error ImageScaler::scale(const ImageBuffer &in, const ImageBuffer &out, MemSpace space) {
        if (!in.isValid() || !out.isValid() || in.format != out.format) return Error::Invalid;
        if (in.format == PixelFormat::Rgb565) return Error::NotSupported;
        const unsigned ch = channelsOf(in.format);
        const unsigned bpp = bytesPerPixel(in.format);
        if (in.width == out.width && in.height == out.height) {
                for (uint32_t y = 0; y < in.height; ++y) std::memcpy(out.row(y), in.row(y), size_t(in.width) * bpp);
                return Error();
        }
        // One allocation: both axes' taps, then the ring of rows filtered across.
        const size_t wx = weightCount(in.width, out.width), wy = weightCount(in.height, out.height);
        const size_t tapBytes = (size_t(out.width) + out.height) * (4 + 2 + 4) + (wx + wy) * 2;
        // The ring's rows: as many as an output row takes at most (known after the taps; bounded here).
        const uint32_t ringMax = out.height >= in.height ? 2 : (in.height + out.height - 1) / out.height + 1;
        const size_t   rowValues = size_t(out.width) * ch;
        const size_t   ringBytes = size_t(ringMax) * rowValues * 2 + size_t(ringMax) * 4 + rowValues * 4;
        auto          *mem = static_cast<uint8_t *>(Memory::alloc(space, tapBytes + ringBytes + 16));
        if (mem == nullptr) return Error::NoMem;
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
        auto *acc = reinterpret_cast<uint32_t *>(take(rowValues * 4, 4));
        auto *ringRow = reinterpret_cast<int32_t *>(take(ringMax * 4u, 4)); // which input row each slot holds
        tx.count = reinterpret_cast<uint16_t *>(take(out.width * 2u, 2));
        ty.count = reinterpret_cast<uint16_t *>(take(out.height * 2u, 2));
        tx.weight = reinterpret_cast<uint16_t *>(take(wx * 2, 2));
        ty.weight = reinterpret_cast<uint16_t *>(take(wy * 2, 2));
        auto *ring = reinterpret_cast<uint16_t *>(take(size_t(ringMax) * rowValues * 2, 2));
        makeTaps(in.width, out.width, tx);
        makeTaps(in.height, out.height, ty);
        for (uint32_t i = 0; i < ringMax; ++i) ringRow[i] = -1;
        // Input row @p j filtered across, from the ring or made there.
        const auto across = [&](uint32_t j) -> const uint16_t * {
                const uint32_t slot = j % ringMax;
                uint16_t      *r = ring + size_t(slot) * rowValues;
                if (ringRow[slot] == int32_t(j)) return r;
                acrossRow(in.row(j), tx, out.width, ch, r);
                ringRow[slot] = int32_t(j);
                return r;
        };
        // Each output row: its input rows' weighted sum, back to 8 bits (acc: value × 16 × One).
        constexpr unsigned Shift = 12 + 4;
        for (uint32_t y = 0; y < out.height; ++y) {
                const uint16_t *w = ty.weight + ty.at[y];
                const uint16_t *r0 = across(ty.first[y]);
                const uint32_t  w0 = w[0];
                for (size_t i = 0; i < rowValues; ++i) acc[i] = r0[i] * w0;
                for (uint32_t k = 1; k < ty.count[y]; ++k) {
                        const uint16_t *r = across(ty.first[y] + k);
                        const uint32_t  wk = w[k];
                        for (size_t i = 0; i < rowValues; ++i) acc[i] += r[i] * wk;
                }
                uint8_t *dst = out.row(y); // a byte a channel: bytes a pixel == channels here
                for (size_t i = 0; i < rowValues; ++i) {
                        const uint32_t v = (acc[i] + (1u << (Shift - 1))) >> Shift;
                        dst[i] = uint8_t(v > 255 ? 255 : v);
                }
        }
        Memory::free(mem);
        return Error();
}

ImageScaler::Size ImageScaler::oriented(uint32_t width, uint32_t height, uint8_t orientation) {
        return orientation >= 5 && orientation <= 8 ? Size{height, width} : Size{width, height};
}

Error ImageScaler::orient(const ImageBuffer &in, const ImageBuffer &out, uint8_t orientation) {
        const Size s = oriented(in.width, in.height, orientation);
        if (!in.isValid() || !out.isValid() || in.format != out.format || out.width != s.width ||
            out.height != s.height)
                return Error::Invalid;
        const unsigned bpp = bytesPerPixel(in.format);
        const uint32_t w = in.width, h = in.height;
        // Output (x, y) takes input (sx, sy): EXIF's eight, each undone.
        for (uint32_t y = 0; y < out.height; ++y) {
                uint8_t *dst = out.row(y);
                for (uint32_t x = 0; x < out.width; ++x) {
                        uint32_t sx = x, sy = y;
                        switch (orientation) {
                                case 2: sx = w - 1 - x; break;                 // mirrored across
                                case 3: sx = w - 1 - x, sy = h - 1 - y; break; // turned half
                                case 4: sy = h - 1 - y; break;                 // mirrored down
                                case 5: sx = y, sy = x; break;                 // transposed
                                case 6: sx = y, sy = h - 1 - x; break;         // turned a quarter right
                                case 7: sx = w - 1 - y, sy = h - 1 - x; break; // transversed
                                case 8: sx = w - 1 - y, sy = x; break;         // turned a quarter left
                                default: break;
                        }
                        std::memcpy(dst + size_t(x) * bpp, in.row(sy) + size_t(sx) * bpp, bpp);
                }
        }
        return Error();
}

BMDS_NAMESPACE_END
