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

        constexpr uint8_t  Signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        constexpr uint32_t MaxSide = 4096;

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
                if (h.width == 0 || h.height == 0 || c.data[10] != 0 || c.data[11] != 0) return Error::CorruptData;
                if (h.width > MaxSide || h.height > MaxSide) return Error::NotSupported;
                if (c.data[12] != 0) return Error::NotSupported; // interlaced
                const bool paletteDepth = h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8;
                switch (h.type) {
                        case Grey:
                        case Rgb:
                        case GreyAlpha:
                        case Rgba: return h.depth == 8 ? Error() : Error(Error::NotSupported);
                        case Palette: return paletteDepth ? Error() : Error(Error::CorruptData);
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

        // Undoes each row's filter in place: @p raw is (1 + @p stride) bytes a row.
        Error unfilter(uint8_t *raw, uint32_t rows, size_t stride, unsigned bpp) {
                const uint8_t *prev = nullptr;
                for (uint32_t y = 0; y < rows; ++y) {
                        const uint8_t filter = raw[0];
                        uint8_t      *row = raw + 1;
                        for (size_t x = 0; x < stride; ++x) {
                                const int a = x >= bpp ? row[x - bpp] : 0;
                                const int b = prev != nullptr ? prev[x] : 0;
                                const int c = prev != nullptr && x >= bpp ? prev[x - bpp] : 0;
                                switch (filter) {
                                        case 0: break;
                                        case 1: row[x] = uint8_t(row[x] + a); break;
                                        case 2: row[x] = uint8_t(row[x] + b); break;
                                        case 3: row[x] = uint8_t(row[x] + ((a + b) >> 1)); break;
                                        case 4: row[x] = uint8_t(row[x] + paeth(a, b, c)); break;
                                        default: return Error::CorruptData;
                                }
                        }
                        prev = row;
                        raw += 1 + stride;
                }
                return Error();
        }

} // namespace

uint32_t Png::crc32(ByteSpan data, uint32_t crc) {
        crc = ~crc;
        for (const uint8_t byte : data) {
                crc ^= byte;
                for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        return ~crc;
}

Result<Png::Size> Png::size(ByteSpan file) {
        Header      h;
        const Error err = header(file, h);
        if (err.isError()) return makeError<Size>(err);
        return makeResult(Size{uint16_t(h.width), uint16_t(h.height)});
}

Result<Png::Size> Png::decode(ByteSpan file, Span<uint32_t> pixels, MemSpace space) {
        Header h;
        Error  err = header(file, h);
        if (err.isError()) return makeError<Size>(err);
        if (pixels.size() < size_t(h.width) * h.height) return makeError<Size>(Error::BufferTooSmall);
        // The chunks: the palette, its alpha, and the image data's total size.
        uint32_t palette[256] = {};
        size_t   paletteCount = 0;
        bool     keyed = false; // a tRNS colour key for grey or RGB
        uint16_t key[3] = {};
        size_t   compressed = 0;
        size_t   at = 8;
        bool     ended = false;
        while (!ended) {
                Chunk c;
                err = next(file, at, c);
                if (err.isError()) return makeError<Size>(err);
                if (c.type == tag("PLTE")) {
                        if (c.length % 3 != 0 || c.length / 3 > 256) return makeError<Size>(Error::CorruptData);
                        paletteCount = c.length / 3;
                        for (size_t i = 0; i < paletteCount; ++i)
                                palette[i] = 0xff000000u | uint32_t(c.data[3 * i]) << 16 |
                                             uint32_t(c.data[3 * i + 1]) << 8 | c.data[3 * i + 2];
                } else if (c.type == tag("tRNS")) {
                        if (h.type == Palette) {
                                for (size_t i = 0; i < c.length && i < 256; ++i)
                                        palette[i] = (palette[i] & 0x00ffffffu) | uint32_t(c.data[i]) << 24;
                        } else if (h.type == Grey && c.length == 2) {
                                keyed = true;
                                key[0] = uint16_t(c.data[0] << 8 | c.data[1]);
                        } else if (h.type == Rgb && c.length == 6) {
                                keyed = true;
                                for (int i = 0; i < 3; ++i) key[i] = uint16_t(c.data[2 * i] << 8 | c.data[2 * i + 1]);
                        }
                } else if (c.type == tag("IDAT")) {
                        compressed += c.length;
                } else if (c.type == tag("IEND")) {
                        ended = true;
                } else if (c.type != tag("IHDR") && (c.type >> 24 & 0x20) == 0) {
                        return makeError<Size>(Error::NotSupported); // a critical chunk not known
                }
        }
        if (compressed == 0 || (h.type == Palette && paletteCount == 0)) return makeError<Size>(Error::CorruptData);
        const unsigned bitsPerPixel = channels(h.type) * h.depth;
        const size_t   stride = (size_t(h.width) * bitsPerPixel + 7) / 8;
        const size_t   rawBytes = (1 + stride) * h.height;
        auto          *work = static_cast<uint8_t *>(Memory::alloc(space, compressed + rawBytes));
        if (work == nullptr) return makeError<Size>(Error::NoMem);
        // The IDAT chunks' data together, then inflated after it.
        size_t gathered = 0;
        at = 8;
        for (Chunk c; next(file, at, c).isOk() && c.type != tag("IEND");) {
                if (c.type != tag("IDAT")) continue;
                std::memcpy(work + gathered, c.data, c.length);
                gathered += c.length;
        }
        uint8_t *raw = work + compressed;
        auto [n, ierr] = Inflate::zlib(ByteSpan(work, compressed), WritableByteSpan(raw, rawBytes));
        err = ierr.isError()  ? (ierr == Error::BufferTooSmall ? Error(Error::CorruptData) : ierr)
              : n != rawBytes ? Error(Error::CorruptData)
                              : unfilter(raw, h.height, stride, (bitsPerPixel + 7) / 8);
        if (err.isOk()) {
                uint32_t *out = pixels.data();
                for (uint32_t y = 0; y < h.height; ++y) {
                        const uint8_t *row = raw + y * (1 + stride) + 1;
                        for (uint32_t x = 0; x < h.width; ++x) {
                                uint32_t argb = 0;
                                switch (h.type) {
                                        case Grey: {
                                                const uint8_t v = row[x];
                                                argb = (keyed && v == key[0] ? 0u : 0xff000000u) | uint32_t(v) << 16 |
                                                       uint32_t(v) << 8 | v;
                                                break;
                                        }
                                        case GreyAlpha: {
                                                const uint8_t v = row[2 * x];
                                                argb = uint32_t(row[2 * x + 1]) << 24 | uint32_t(v) << 16 |
                                                       uint32_t(v) << 8 | v;
                                                break;
                                        }
                                        case Rgb: {
                                                const uint8_t *p = row + 3 * x;
                                                const bool     clear =
                                                        keyed && p[0] == key[0] && p[1] == key[1] && p[2] == key[2];
                                                argb = (clear ? 0u : 0xff000000u) | uint32_t(p[0]) << 16 |
                                                       uint32_t(p[1]) << 8 | p[2];
                                                break;
                                        }
                                        case Rgba: {
                                                const uint8_t *p = row + 4 * x;
                                                argb = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 |
                                                       uint32_t(p[1]) << 8 | p[2];
                                                break;
                                        }
                                        default: { // a palette index, 1 to 8 bits, the leftmost pixel in the high bits
                                                const unsigned bit = x * h.depth;
                                                const unsigned shift = 8 - h.depth - bit % 8;
                                                const unsigned index = (row[bit / 8] >> shift) & ((1u << h.depth) - 1);
                                                if (index >= paletteCount) {
                                                        err = Error::CorruptData;
                                                        break;
                                                }
                                                argb = palette[index];
                                                break;
                                        }
                                }
                                *out++ = argb;
                        }
                }
        }
        Memory::free(work);
        if (err.isError()) return makeError<Size>(err);
        return makeResult(Size{uint16_t(h.width), uint16_t(h.height)});
}

BMDS_NAMESPACE_END
