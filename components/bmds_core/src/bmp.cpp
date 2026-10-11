/**
 * @file      bmp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Windows bitmaps (devplan/images.md §3b).
 */

#include <cstring>
#include <bmds/bmp.h>

BMDS_NAMESPACE_BEGIN

namespace {

        enum Compression : uint32_t {
                Rgb = 0,
                Rle8 = 1,
                Rle4 = 2,
                BitFields = 3,
                Jpeg = 4,
                PngInside = 5,
                AlphaBitFields = 6,
        };

        uint32_t le16(const uint8_t *p) {
                return uint32_t(p[0]) | uint32_t(p[1]) << 8;
        }

        uint32_t le32(const uint8_t *p) {
                return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        // A channel by its mask: where it starts and how many bits it has.
        struct Channel {
                        uint32_t mask = 0;
                        unsigned shift = 0;
                        unsigned bits = 0;

                        void set(uint32_t m) {
                                mask = m;
                                shift = 0;
                                bits = 0;
                                if (m == 0) return;
                                while ((m & 1u) == 0) {
                                        m >>= 1;
                                        ++shift;
                                }
                                while ((m & 1u) != 0) {
                                        m >>= 1;
                                        ++bits;
                                }
                                return;
                        }

                        uint32_t eight(uint32_t px) const {
                                if (bits == 0) return 0;
                                const uint32_t v = (px & mask) >> shift;
                                if (bits >= 8) return v >> (bits - 8);
                                return v * 255u / ((1u << bits) - 1);
                        }
        };

        struct Header {
                        uint32_t width = 0;
                        uint32_t height = 0;
                        bool     topDown = false;
                        unsigned bpp = 0;
                        uint32_t compression = Rgb;
                        size_t   pixelsAt = 0;
                        size_t   paletteAt = 0;
                        unsigned paletteEntry = 4; // bytes an entry (3: OS/2)
                        uint32_t colors = 0;       // palette entries
                        Channel  r, g, b, a;
        };

        Error parse(ByteSpan file, Header &h) {
                const uint8_t *p = file.data();
                if (file.size() < 14 + 12 || p[0] != 'B' || p[1] != 'M') return Error::CorruptData;
                h.pixelsAt = le32(p + 10);
                const uint32_t dib = le32(p + 14);
                if (dib < 12 || 14 + size_t(dib) > file.size()) return Error::CorruptData;
                const uint8_t *d = p + 14;
                int32_t        w = 0, ht = 0;
                if (dib == 12) { // OS/2 1.x
                        w = int32_t(le16(d + 4));
                        ht = int32_t(int16_t(le16(d + 6)));
                        h.bpp = le16(d + 10);
                        h.paletteEntry = 3;
                } else {
                        if (dib < 40) return Error::CorruptData;
                        w = int32_t(le32(d + 4));
                        ht = int32_t(le32(d + 8));
                        h.bpp = le16(d + 14);
                        h.compression = le32(d + 16);
                        h.colors = le32(d + 32);
                }
                if (w <= 0 || ht == 0 || ht == INT32_MIN) return Error::CorruptData;
                h.width = uint32_t(w);
                h.topDown = ht < 0;
                h.height = uint32_t(ht < 0 ? -ht : ht);
                if (h.width > Bmp::MaxSide || h.height > Bmp::MaxSide) return Error::NotSupported;
                if (h.compression == Jpeg || h.compression == PngInside) return Error::NotSupported;
                // The masks: in the header (52 bytes and up), or after a 40-byte one.
                size_t after = 14 + dib;
                if (h.compression == BitFields || h.compression == AlphaBitFields) {
                        if (h.bpp != 16 && h.bpp != 32) return Error::CorruptData;
                        const uint8_t *m = d + 40;
                        const unsigned count = h.compression == AlphaBitFields ? 4 : 3;
                        if (dib == 40) {
                                if (after + 4 * count > file.size()) return Error::CorruptData;
                                m = p + after;
                                after += 4 * count;
                        } else if (dib < 40 + 4 * count) {
                                return Error::CorruptData;
                        }
                        h.r.set(le32(m));
                        h.g.set(le32(m + 4));
                        h.b.set(le32(m + 8));
                        if (count == 4 || dib >= 56) h.a.set(le32(m + 12));
                } else if (h.compression == Rgb && (h.bpp == 16 || h.bpp == 32 || h.bpp == 24)) {
                        if (h.bpp == 16) {
                                h.r.set(0x7c00);
                                h.g.set(0x03e0);
                                h.b.set(0x001f);
                        } else {
                                h.r.set(0xff0000);
                                h.g.set(0x00ff00);
                                h.b.set(0x0000ff);
                        }
                } else if (h.compression == Rle8   ? h.bpp != 8
                           : h.compression == Rle4 ? h.bpp != 4
                                                   : h.compression != Rgb || (h.bpp != 1 && h.bpp != 4 && h.bpp != 8)) {
                        return h.compression > AlphaBitFields ? Error::NotSupported : Error::CorruptData;
                }
                if (h.bpp <= 8) {
                        const uint32_t most = 1u << h.bpp;
                        if (h.colors == 0 || h.colors > most) h.colors = most;
                        h.paletteAt = after;
                        if (h.paletteAt + size_t(h.colors) * h.paletteEntry > file.size()) {
                                // Fewer entries than said: as many as there are before the pixels.
                                if (h.pixelsAt <= h.paletteAt) return Error::CorruptData;
                                h.colors = uint32_t((h.pixelsAt - h.paletteAt) / h.paletteEntry);
                                if (h.colors == 0) return Error::CorruptData;
                        }
                }
                if (h.pixelsAt >= file.size()) return Error::CorruptData;
                if (h.compression != Rle8 && h.compression != Rle4) {
                        const uint64_t stride = (uint64_t(h.width) * h.bpp + 31) / 32 * 4;
                        // The last row needn't be padded out.
                        const uint64_t need = stride * (h.height - 1) + (uint64_t(h.width) * h.bpp + 7) / 8;
                        if (h.pixelsAt + need > file.size()) return Error::CorruptData;
                }
                return Error();
        }

        uint32_t entry(const ByteSpan file, const Header &h, unsigned index) {
                if (index >= h.colors) return 0xff000000u;
                const uint8_t *e = file.data() + h.paletteAt + size_t(index) * h.paletteEntry;
                return 0xff000000u | uint32_t(e[2]) << 16 | uint32_t(e[1]) << 8 | e[0];
        }

        // RLE8 or RLE4 into @p out (an index a pixel, rows bottom-up as the file's).
        Error unpackRle(ByteSpan file, const Header &h, const ImageBuffer &out, uint32_t background) {
                const uint8_t *p = file.data() + h.pixelsAt;
                const uint8_t *end = file.data() + file.size();
                uint32_t       x = 0, y = 0; // y counts from the file's first row
                uint32_t       row[64];      // a run's colours, converted 64 at a time
                const auto     put = [&](unsigned index, uint32_t count) {
                        while (count > 0 && x < h.width) {
                                uint32_t n = 0;
                                for (; n < count && n < 64 && x + n < h.width; ++n)
                                        row[n] = entry(file, h,
                                                       h.compression == Rle4 ? (n % 2 == 0 ? index >> 4 : index & 15)
                                                                                 : index);
                                const uint32_t outY = h.topDown ? y : h.height - 1 - y;
                                if (y < h.height)
                                        Pixels::fromArgb(row, n, out.row(outY) + x * bytesPerPixel(out.format),
                                                             out.format, background);
                                x += n;
                                count -= n;
                                if (h.compression == Rle4 && n % 2 != 0) index = (index << 4 | index >> 4) & 0xff;
                        }
                        return;
                };
                // Unset pixels are entry 0.
                for (uint32_t r = 0; r < h.height; ++r) {
                        x = 0;
                        y = r;
                        put(0, h.width);
                }
                x = 0;
                y = 0;
                while (p + 2 <= end && y < h.height) {
                        const uint8_t count = p[0], code = p[1];
                        p += 2;
                        if (count != 0) {
                                put(code, count);
                                continue;
                        }
                        if (code == 0) { // end of a line
                                x = 0;
                                ++y;
                        } else if (code == 1) { // end of the bitmap
                                return Error();
                        } else if (code == 2) { // a move
                                if (p + 2 > end) return Error::CorruptData;
                                x += p[0];
                                y += p[1];
                                p += 2;
                        } else { // `code` literal pixels, padded to 16 bits
                                const size_t bytes = h.compression == Rle4 ? (code + 1u) / 2 : code;
                                if (p + bytes > end) return Error::CorruptData;
                                for (unsigned i = 0; i < code; ++i) {
                                        const unsigned index = h.compression == Rle4
                                                                       ? (i % 2 == 0 ? p[i / 2] >> 4 : p[i / 2] & 15)
                                                                       : p[i];
                                        put(h.compression == Rle4 ? index << 4 : index, 1);
                                }
                                p += (bytes + 1) & ~size_t(1);
                        }
                }
                return Error();
        }

} // namespace

Result<ImageInfo> Bmp::info(ByteSpan file) {
        Header      h;
        const Error err = parse(file, h);
        if (err.isError()) return makeError<ImageInfo>(err);
        ImageInfo info;
        info.codec = ImageCodec::Bmp;
        info.width = h.width;
        info.height = h.height;
        info.flags = h.a.bits != 0 ? ImageInfo::HasAlpha : 0;
        return makeResult(info);
}

Error Bmp::decode(ByteSpan file, const ImageBuffer &out, uint32_t background) {
        Header h;
        Error  err = parse(file, h);
        if (err.isError()) return err;
        if (out.pixels == nullptr || out.width < h.width || out.height < h.height ||
            out.stride < size_t(h.width) * bytesPerPixel(out.format))
                return Error::BufferTooSmall;
        if (h.compression == Rle8 || h.compression == Rle4) return unpackRle(file, h, out, background);
        const size_t stride = (size_t(h.width) * h.bpp + 31) / 32 * 4;
        uint32_t     argb[64];
        uint32_t     palette[256];
        for (unsigned i = 0; h.bpp <= 8 && i < (1u << h.bpp); ++i) palette[i] = entry(file, h, i);
        for (uint32_t y = 0; y < h.height; ++y) {
                const uint8_t *row = file.data() + h.pixelsAt + size_t(h.topDown ? y : h.height - 1 - y) * stride;
                uint8_t       *to = out.row(y);
                for (uint32_t x0 = 0; x0 < h.width; x0 += 64) {
                        const uint32_t n = h.width - x0 < 64 ? h.width - x0 : 64;
                        for (uint32_t i = 0; i < n; ++i) {
                                const uint32_t x = x0 + i;
                                uint32_t       c = 0;
                                switch (h.bpp) {
                                        case 1: c = palette[row[x / 8] >> (7 - x % 8) & 1]; break;
                                        case 4: c = palette[x % 2 == 0 ? row[x / 2] >> 4 : row[x / 2] & 15]; break;
                                        case 8: c = palette[row[x]]; break;
                                        case 24:
                                                c = 0xff000000u | uint32_t(row[3 * x + 2]) << 16 |
                                                    uint32_t(row[3 * x + 1]) << 8 | row[3 * x];
                                                break;
                                        default: {
                                                const uint32_t px = h.bpp == 16 ? le16(row + 2 * x) : le32(row + 4 * x);
                                                const uint32_t alpha = h.a.bits != 0 ? h.a.eight(px) : 0xff;
                                                c = alpha << 24 | h.r.eight(px) << 16 | h.g.eight(px) << 8 |
                                                    h.b.eight(px);
                                                break;
                                        }
                                }
                                argb[i] = c;
                        }
                        Pixels::fromArgb(argb, n, to + size_t(x0) * bytesPerPixel(out.format), out.format, background);
                }
        }
        return Error();
}

BMDS_NAMESPACE_END
