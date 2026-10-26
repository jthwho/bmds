/**
 * @file      pngdecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/inflate.h>
#include <bmds/memory.h>
#include <bmds/pngdecoder.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t Signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        enum ColorType : uint8_t {
                Grey = 0,
                Rgb = 2,
                Palette = 3,
                GreyAlpha = 4,
                Rgba = 6
        };

        uint32_t be32(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

        struct Header {
                        uint32_t width = 0;
                        uint32_t height = 0;
                        uint8_t  depth = 0;
                        uint8_t  type = 0;
                        bool     interlaced = false;
        };

        // A chunk at @p at of @p file (its CRC checked), or false at a bad one.
        struct Chunk {
                        uint32_t       type = 0;
                        const uint8_t *data = nullptr;
                        uint32_t       length = 0;
        };

        constexpr uint32_t tag(const char (&s)[5]) {
                return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
                       uint8_t(s[3]);
        }

        Error next(ByteSpan file, size_t &at, Chunk &out) {
                if (file.size() - at < 12) return Error::CorruptData;
                const uint32_t length = be32(file.data() + at);
                if (length > file.size() - at - 12) return Error::CorruptData;
                const uint8_t *type = file.data() + at + 4;
                const uint32_t crc = be32(type + 4 + length);
                if (Png::crc32(ByteSpan(type, length + 4)) != crc) return Error::CorruptData;
                out = Chunk{be32(type), type + 4, length};
                at += 12 + length;
                return Error();
        }

        // The header, checked for what decode() can do.
        Error header(ByteSpan file, Header &h) {
                if (file.size() < 8 + 25 || std::memcmp(file.data(), Signature, 8) != 0) return Error::CorruptData;
                size_t at = 8;
                Chunk  c;
                Error  err = next(file, at, c);
                if (err.isError()) return err;
                if (c.type != tag("IHDR") || c.length != 13) return Error::CorruptData;
                h.width = be32(c.data);
                h.height = be32(c.data + 4);
                h.depth = c.data[8];
                h.type = c.data[9];
                h.interlaced = c.data[12] == 1;
                if (h.width == 0 || h.height == 0 || c.data[10] != 0 || c.data[11] != 0 || c.data[12] > 1)
                        return Error::CorruptData;
                if (h.width > Png::MaxSide || h.height > Png::MaxSide) return Error::NotSupported;
                const bool lowDepth = h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8;
                switch (h.type) {
                        case Grey: return lowDepth || h.depth == 16 ? Error() : Error(Error::CorruptData);
                        case Rgb:
                        case GreyAlpha:
                        case Rgba: return h.depth == 8 || h.depth == 16 ? Error() : Error(Error::CorruptData);
                        case Palette: return lowDepth ? Error() : Error(Error::CorruptData);
                        default: return Error::CorruptData;
                }
        }

        unsigned channels(uint8_t type) {
                switch (type) {
                        case Grey:
                        case Palette: return 1;
                        case GreyAlpha: return 2;
                        case Rgb: return 3;
                        default: return 4;
                }
        }

        uint8_t paeth(int a, int b, int c) {
                const int p = a + b - c;
                const int pa = p > a ? p - a : a - p;
                const int pb = p > b ? p - b : b - p;
                const int pc = p > c ? p - c : c - p;
                if (pa <= pb && pa <= pc) return uint8_t(a);
                return uint8_t(pb <= pc ? b : c);
        }

        // Undoes each row's filter in place: @p raw is (1 + @p stride) bytes a row. A loop for each filter,
        // the first pixel's bytes (no left neighbour) apart.
        Error unfilter(uint8_t *raw, uint32_t rows, size_t stride, unsigned bpp) {
                const uint8_t *prev = nullptr;
                const size_t   lead = bpp < stride ? bpp : stride;
                for (uint32_t y = 0; y < rows; ++y) {
                        const uint8_t filter = raw[0];
                        uint8_t      *row = raw + 1;
                        switch (filter) {
                                case 0: break;
                                case 1:
                                        for (size_t x = bpp; x < stride; ++x) row[x] = uint8_t(row[x] + row[x - bpp]);
                                        break;
                                case 2:
                                        if (prev != nullptr)
                                                for (size_t x = 0; x < stride; ++x) row[x] = uint8_t(row[x] + prev[x]);
                                        break;
                                case 3:
                                        if (prev == nullptr) {
                                                for (size_t x = bpp; x < stride; ++x)
                                                        row[x] = uint8_t(row[x] + (row[x - bpp] >> 1));
                                        } else {
                                                for (size_t x = 0; x < lead; ++x)
                                                        row[x] = uint8_t(row[x] + (prev[x] >> 1));
                                                for (size_t x = bpp; x < stride; ++x)
                                                        row[x] = uint8_t(row[x] + ((row[x - bpp] + prev[x]) >> 1));
                                        }
                                        break;
                                case 4:
                                        if (prev == nullptr) { // Paeth with no row above: Sub
                                                for (size_t x = bpp; x < stride; ++x)
                                                        row[x] = uint8_t(row[x] + row[x - bpp]);
                                        } else {
                                                for (size_t x = 0; x < lead; ++x) row[x] = uint8_t(row[x] + prev[x]);
                                                for (size_t x = bpp; x < stride; ++x)
                                                        row[x] = uint8_t(row[x] +
                                                                         paeth(row[x - bpp], prev[x], prev[x - bpp]));
                                        }
                                        break;
                                default: return Error::CorruptData;
                        }
                        prev = row;
                        raw += 1 + stride;
                }
                return Error();
        }

} // namespace

namespace {

        // CRC-32's table (rodata, made at compile time): a byte at a time.
        struct CrcTable {
                        uint32_t v[256];
        };

        constexpr CrcTable makeCrcTable() {
                CrcTable t{};
                for (uint32_t i = 0; i < 256; ++i) {
                        uint32_t c = i;
                        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
                        t.v[i] = c;
                }
                return t;
        }

        constexpr CrcTable Crc = makeCrcTable();

} // namespace

uint32_t Png::crc32(ByteSpan data, uint32_t crc) {
        crc = ~crc;
        for (const uint8_t byte : data) crc = Crc.v[(crc ^ byte) & 0xff] ^ (crc >> 8);
        return ~crc;
}

Result<Png::Size> Png::size(ByteSpan file) {
        Header      h;
        const Error err = header(file, h);
        if (err.isError()) return makeError<Size>(err);
        return makeResult(Size{uint16_t(h.width), uint16_t(h.height)});
}

Result<ImageInfo> Png::info(ByteSpan file) {
        Header      h;
        const Error err = header(file, h);
        if (err.isError()) return makeError<ImageInfo>(err);
        ImageInfo info;
        info.codec = ImageCodec::Png;
        info.width = h.width;
        info.height = h.height;
        // Alpha: its own channel, or a tRNS chunk before the data.
        bool   alpha = h.type == GreyAlpha || h.type == Rgba;
        size_t at = 8;
        for (Chunk c; !alpha && next(file, at, c).isOk() && c.type != tag("IDAT") && c.type != tag("IEND");)
                alpha = c.type == tag("tRNS");
        info.flags = uint8_t((alpha ? ImageInfo::HasAlpha : 0) | (h.interlaced ? ImageInfo::Progressive : 0));
        return makeResult(info);
}

Result<Png::Size> Png::decode(ByteSpan file, Span<uint32_t> pixels, MemSpace space) {
        Header      h;
        const Error herr = header(file, h);
        if (herr.isError()) return makeError<Size>(herr);
        if (pixels.size() < size_t(h.width) * h.height) return makeError<Size>(Error::BufferTooSmall);
        ImageBuffer out;
        out.pixels = reinterpret_cast<uint8_t *>(pixels.data()); // little-endian: B, G, R, A
        out.width = h.width;
        out.height = h.height;
        out.stride = size_t(h.width) * 4;
        out.format = PixelFormat::Bgra32;
        const Error err = decode(file, out, 0, space);
        if (err.isError()) return makeError<Size>(err);
        return makeResult(Size{uint16_t(h.width), uint16_t(h.height)});
}

namespace {

        // What turns a row of samples into 0xAARRGGBB.
        struct Samples {
                        const Header *h = nullptr;
                        uint32_t      palette[256] = {};
                        size_t        paletteCount = 0;
                        bool          keyed = false; // a tRNS colour key for grey or RGB
                        uint16_t      key[3] = {};
        };

        // Sample @p i of @p row (@p depth bits each, the leftmost in the high bits).
        unsigned sample(const uint8_t *row, size_t i, unsigned depth) {
                if (depth == 8) return row[i];
                if (depth == 16) return unsigned(row[2 * i]) << 8 | row[2 * i + 1];
                const size_t   bit = i * depth;
                const unsigned shift = 8 - depth - bit % 8;
                return (row[bit / 8] >> shift) & ((1u << depth) - 1);
        }

        Error toArgb(const Samples &s, const uint8_t *row, uint32_t *out) {
                const Header  &h = *s.h;
                const unsigned d = h.depth;
                // The common kinds, quickly: 8-bit RGB (no key) and RGBA.
                if (d == 8 && h.type == Rgba) {
                        for (uint32_t x = 0; x < h.width; ++x, row += 4)
                                out[x] = uint32_t(row[3]) << 24 | uint32_t(row[0]) << 16 | uint32_t(row[1]) << 8 |
                                         row[2];
                        return Error();
                }
                if (d == 8 && h.type == Rgb && !s.keyed) {
                        for (uint32_t x = 0; x < h.width; ++x, row += 3)
                                out[x] = 0xff000000u | uint32_t(row[0]) << 16 | uint32_t(row[1]) << 8 | row[2];
                        return Error();
                }
                const unsigned top = (1u << d) - 1; // the largest sample
                const auto     eight = [d, top](unsigned v) {
                        return d == 16 ? v >> 8 : d == 8 ? v : v * 255 / top;
                };
                for (uint32_t x = 0; x < h.width; ++x) {
                        uint32_t argb = 0;
                        switch (h.type) {
                                case Grey: {
                                        const unsigned raw = sample(row, x, d);
                                        const uint32_t v = eight(raw);
                                        argb = (s.keyed && raw == s.key[0] ? 0u : 0xff000000u) | v * 0x010101u;
                                        break;
                                }
                                case GreyAlpha: {
                                        const uint32_t v = eight(sample(row, 2 * x, d));
                                        argb = eight(sample(row, 2 * x + 1, d)) << 24 | v * 0x010101u;
                                        break;
                                }
                                case Rgb: {
                                        const unsigned r = sample(row, 3 * x, d), g = sample(row, 3 * x + 1, d),
                                                       b = sample(row, 3 * x + 2, d);
                                        const bool clear = s.keyed && r == s.key[0] && g == s.key[1] && b == s.key[2];
                                        argb = (clear ? 0u : 0xff000000u) | eight(r) << 16 | eight(g) << 8 | eight(b);
                                        break;
                                }
                                case Rgba:
                                        argb = eight(sample(row, 4 * x + 3, d)) << 24 |
                                               eight(sample(row, 4 * x, d)) << 16 |
                                               eight(sample(row, 4 * x + 1, d)) << 8 | eight(sample(row, 4 * x + 2, d));
                                        break;
                                default: { // a palette index
                                        const unsigned index = sample(row, x, d);
                                        if (index >= s.paletteCount) return Error::CorruptData;
                                        argb = s.palette[index];
                                        break;
                                }
                        }
                        out[x] = argb;
                }
                return Error();
        }

        // Adam7's passes: where each starts and its steps.
        struct Pass {
                        uint8_t x0, y0, dx, dy;
        };
        constexpr Pass Passes[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

        uint32_t passSide(uint32_t side, uint8_t start, uint8_t step) {
                return side > start ? (side - start + step - 1) / step : 0;
        }

        // Copies pixel @p from of @p src to pixel @p to of @p dst (@p bpp bits each).
        void copyPixel(const uint8_t *src, size_t from, uint8_t *dst, size_t to, unsigned bpp) {
                if (bpp >= 8) {
                        std::memcpy(dst + to * (bpp / 8), src + from * (bpp / 8), bpp / 8);
                        return;
                }
                const unsigned v = sample(src, from, bpp);
                const size_t   bit = to * bpp;
                const unsigned shift = 8 - bpp - bit % 8;
                const unsigned mask = ((1u << bpp) - 1) << shift;
                dst[bit / 8] = uint8_t((dst[bit / 8] & ~mask) | (v << shift));
                return;
        }

} // namespace

Result<size_t> Png::palette(ByteSpan file, Span<uint32_t> out) {
        Header      h;
        const Error herr = header(file, h);
        if (herr.isError()) return makeError<size_t>(herr);
        if (h.type != Palette) return makeResult(size_t(0));
        size_t at = 8;
        for (Chunk c; next(file, at, c).isOk() && c.type != tag("IDAT") && c.type != tag("IEND");) {
                if (c.type != tag("PLTE")) continue;
                if (c.length % 3 != 0 || c.length / 3 > 256) break;
                const size_t n = c.length / 3;
                for (size_t i = 0; i < n && i < out.size(); ++i)
                        out[i] = uint32_t(c.data[3 * i]) << 16 | uint32_t(c.data[3 * i + 1]) << 8 | c.data[3 * i + 2];
                return makeResult(n);
        }
        return makeError<size_t>(Error::CorruptData);
}

bool Png::nextText(ByteSpan file, size_t &at, StringView &keyword, StringView &text) {
        if (at < 8) at = 8;
        for (Chunk c; at < file.size() && next(file, at, c).isOk() && c.type != tag("IEND");) {
                if (c.type != tag("tEXt")) continue;
                const auto  *chars = reinterpret_cast<const char *>(c.data);
                const size_t nul = StringView(chars, c.length).indexOf('\0');
                if (nul == StringView::NPos || nul == 0) continue; // no keyword
                keyword = StringView(chars, nul);
                text = StringView(chars + nul + 1, c.length - nul - 1);
                return true;
        }
        return false;
}

Error Png::decode(ByteSpan file, const ImageBuffer &out, uint32_t background, MemSpace space,
                  Span<const uint32_t> colors) {
        Header h;
        Error  err = header(file, h);
        if (err.isError()) return err;
        if (out.pixels == nullptr || out.width < h.width || out.height < h.height ||
            out.stride < size_t(h.width) * bytesPerPixel(out.format))
                return Error::BufferTooSmall;
        // The chunks: the palette, its alpha, and the image data's total size.
        Samples s;
        s.h = &h;
        size_t compressed = 0;
        size_t at = 8;
        bool   ended = false;
        while (!ended) {
                Chunk c;
                err = next(file, at, c);
                if (err.isError()) return err;
                if (c.type == tag("PLTE")) {
                        if (c.length % 3 != 0 || c.length / 3 > 256) return Error::CorruptData;
                        s.paletteCount = c.length / 3;
                        for (size_t i = 0; i < s.paletteCount; ++i)
                                s.palette[i] = 0xff000000u | uint32_t(c.data[3 * i]) << 16 |
                                               uint32_t(c.data[3 * i + 1]) << 8 | c.data[3 * i + 2];
                } else if (c.type == tag("tRNS")) {
                        if (h.type == Palette) {
                                for (size_t i = 0; i < c.length && i < 256; ++i)
                                        s.palette[i] = (s.palette[i] & 0x00ffffffu) | uint32_t(c.data[i]) << 24;
                        } else if (h.type == Grey && c.length == 2) {
                                s.keyed = true;
                                s.key[0] = uint16_t(c.data[0] << 8 | c.data[1]);
                        } else if (h.type == Rgb && c.length == 6) {
                                s.keyed = true;
                                for (int i = 0; i < 3; ++i) s.key[i] = uint16_t(c.data[2 * i] << 8 | c.data[2 * i + 1]);
                        }
                } else if (c.type == tag("IDAT")) {
                        compressed += c.length;
                } else if (c.type == tag("IEND")) {
                        ended = true;
                } else if (c.type != tag("IHDR") && (c.type >> 24 & 0x20) == 0) {
                        return Error::NotSupported; // a critical chunk not known
                }
        }
        if (compressed == 0 || (h.type == Palette && s.paletteCount == 0)) return Error::CorruptData;
        if (h.type == Palette)
                for (size_t i = 0; i < colors.size() && i < s.paletteCount; ++i)
                        s.palette[i] = (s.palette[i] & 0xff000000u) | (colors[i] & 0x00ffffffu);
        const unsigned bpp = channels(h.type) * h.depth; // bits a pixel
        const unsigned filterBpp = (bpp + 7) / 8;
        const size_t   stride = (size_t(h.width) * bpp + 7) / 8;
        // The raw (filtered) bytes: one image, or Adam7's seven.
        uint64_t rawBytes = 0;
        if (h.interlaced) {
                for (const Pass &p : Passes) {
                        const uint32_t pw = passSide(h.width, p.x0, p.dx), ph = passSide(h.height, p.y0, p.dy);
                        if (pw != 0 && ph != 0) rawBytes += uint64_t(ph) * (1 + (uint64_t(pw) * bpp + 7) / 8);
                }
        } else {
                rawBytes = uint64_t(h.height) * (1 + stride);
        }
        const uint64_t flatBytes = h.interlaced ? uint64_t(h.height) * stride : 0;
        const uint64_t argbAt = (compressed + rawBytes + flatBytes + 3) & ~uint64_t(3); // a row of 0xAARRGGBB
        const uint64_t work64 = argbAt + uint64_t(h.width) * 4;
        if (work64 > SIZE_MAX / 2) return Error::NoMem;
        auto *work = static_cast<uint8_t *>(Memory::alloc(space, size_t(work64)));
        if (work == nullptr) return Error::NoMem;
        // The IDAT chunks' data together, then inflated after it.
        size_t gathered = 0;
        at = 8;
        for (Chunk c; next(file, at, c).isOk() && c.type != tag("IEND");) {
                if (c.type != tag("IDAT")) continue;
                std::memcpy(work + gathered, c.data, c.length);
                gathered += c.length;
        }
        uint8_t *raw = work + compressed;
        uint8_t *flat = raw + rawBytes;
        auto    *argb = reinterpret_cast<uint32_t *>(work + argbAt);
        auto [n, ierr] = Inflate::zlib(ByteSpan(work, compressed), WritableByteSpan(raw, size_t(rawBytes)));
        err = ierr.isError()  ? (ierr == Error::BufferTooSmall ? Error(Error::CorruptData) : ierr)
              : n != rawBytes ? Error(Error::CorruptData)
                              : Error();
        // Each row's filter undone; Adam7's passes put in place in `flat`.
        if (err.isOk() && !h.interlaced) err = unfilter(raw, h.height, stride, filterBpp);
        if (err.isOk() && h.interlaced) {
                std::memset(flat, 0, size_t(flatBytes));
                uint8_t *pass = raw;
                for (const Pass &p : Passes) {
                        const uint32_t pw = passSide(h.width, p.x0, p.dx), ph = passSide(h.height, p.y0, p.dy);
                        if (pw == 0 || ph == 0) continue;
                        const size_t ps = (size_t(pw) * bpp + 7) / 8;
                        err = unfilter(pass, ph, ps, filterBpp);
                        if (err.isError()) break;
                        for (uint32_t y = 0; y < ph; ++y) {
                                const uint8_t *src = pass + y * (1 + ps) + 1;
                                uint8_t       *dst = flat + size_t(p.y0 + y * p.dy) * stride;
                                for (uint32_t x = 0; x < pw; ++x) copyPixel(src, x, dst, p.x0 + x * p.dx, bpp);
                        }
                        pass += ph * (1 + ps);
                }
        }
        for (uint32_t y = 0; err.isOk() && y < h.height; ++y) {
                const uint8_t *row = h.interlaced ? flat + size_t(y) * stride : raw + y * (1 + stride) + 1;
                // 8-bit RGB into B, G, R or R, G, B: straight, no 0xAARRGGBB between.
                if (h.depth == 8 && h.type == Rgb && !s.keyed && out.format == PixelFormat::Bgr24) {
                        Pixels::swap24(row, out.row(y), h.width);
                        continue;
                }
                if (h.depth == 8 && h.type == Rgb && !s.keyed && out.format == PixelFormat::Rgb24) {
                        std::memcpy(out.row(y), row, size_t(h.width) * 3);
                        continue;
                }
                err = toArgb(s, row, argb);
                if (err.isOk()) Pixels::fromArgb(argb, h.width, out.row(y), out.format, background);
        }
        Memory::free(work);
        return err;
}

BMDS_NAMESPACE_END
