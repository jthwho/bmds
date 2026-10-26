/**
 * @file      tiledpattern.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop's pattern: a PNG tile, recolored and scaled, made into rows (devplan/wallpaper.md
 * §4b, §4d).
 */

#include <cstring>
#include <bmds/image.h>
#include <bmds/memory.h>
#include <bmds/pngdecoder.h>

#include <bmds/tiledpattern.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        constexpr size_t TileAlign = 128; // the L2 cache line: the 2D-DMA reads it from PSRAM

        // @p v modulo @p n, never negative.
        uint32_t wrap(int v, uint32_t n) {
                const int m = v % int(n);
                return uint32_t(m < 0 ? m + int(n) : m);
        }

} // namespace

Result<size_t> TiledPattern::parseColors(StringView text, Span<uint32_t> palette) {
        size_t n = 0;
        size_t at = 0;
        while (at <= text.size() && !text.isEmpty()) {
                size_t end = at;
                while (end < text.size() && text[end] != ';') ++end;
                StringView entry = text.mid(at, end - at).trimmed();
                if (!entry.isEmpty()) {
                        if (entry.size() != 7 || entry[0] != '#') return makeError<size_t>(Error::Invalid);
                        uint32_t rgb = 0;
                        for (size_t i = 1; i < 7; ++i) {
                                const int d = hexDigit(entry[i]);
                                if (d < 0) return makeError<size_t>(Error::Invalid);
                                rgb = rgb << 4 | uint32_t(d);
                        }
                        if (n < palette.size()) palette[n] = rgb;
                }
                ++n;
                at = end + 1;
        }
        return makeResult(n);
}

Error TiledPattern::load(ByteSpan file, StringView colors, unsigned scale, uint32_t background) {
        reset();
        auto [size, serr] = Png::size(file);
        if (serr.isError()) return serr;
        if (size.width == 0 || size.height == 0 || size.width > MaxSide || size.height > MaxSide || scale == 0 ||
            scale > MaxScale)
                return Error::OutOfRange;
        auto [info, ierr] = Png::info(file);
        if (ierr.isError()) return ierr;
        // The palette's colors, the asked ones over the file's own.
        uint32_t palette[MaxColors];
        auto [entries, perr] = Png::palette(file, Span<uint32_t>(palette, MaxColors));
        if (perr.isError()) return perr;
        if (entries != 0) {
                const Result<size_t> asked = parseColors(colors, Span<uint32_t>(palette, entries));
                if (asked.isError()) return asked.error();
        }
        const size_t pixels = size_t(size.width) * size.height;
        _argb = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, pixels * 4));
        if (_argb == nullptr) return Error::NoMem;
        ImageBuffer out;
        out.pixels = reinterpret_cast<uint8_t *>(_argb); // little-endian: B, G, R, A
        out.width = size.width;
        out.height = size.height;
        out.stride = size_t(size.width) * 4;
        out.format = PixelFormat::Bgra32;
        Error err = Png::decode(file, out, 0, MemSpace::Psram, Span<const uint32_t>(palette, entries));
        if (err.isOk()) {
                _width = size.width;
                _height = size.height;
                _scale = uint8_t(scale);
                _background = background & 0xffffffu;
                err = build();
        }
        if (err.isError()) {
                reset();
                return err;
        }
        if ((info.flags & ImageInfo::HasAlpha) == 0) {
                Memory::free(_argb); // opaque: nothing to blend again
                _argb = nullptr;
        }
        return Error::Ok;
}

// The B, G, R tile from the 0xAARRGGBB pixels: scaled across, its rows repeated to MinRow and
// a multiple of 4; scaled down and repeated to MinTall rows if it stays small enough.
Error TiledPattern::build() {
        _period = uint16_t(_width * _scale);
        uint32_t unit = _period; // the least whole repeats on 4-pixel columns
        while (unit % 4 != 0) unit += _period;
        _rowWidth = uint16_t(unit * ((MinRow + unit - 1u) / unit));
        const size_t   rowBytes = size_t(_rowWidth) * 3;
        const uint32_t tall = uint32_t(_height) * _scale;
        if (rowBytes * tall <= MaxScaledBytes) {
                _repeat = 1;
                _rows = uint16_t(tall * ((MinTall + tall - 1u) / tall));
        } else {
                _repeat = _scale;
                _rows = _height;
        }
        auto *tile = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::Psram, rowBytes * _rows, TileAlign));
        if (tile == nullptr) return Error::NoMem;
        uint8_t plain[MaxSide * 3]; // a row, unscaled
        for (uint32_t r = 0; r < _rows; ++r) {
                uint8_t       *row = tile + r * rowBytes;
                const uint32_t y = _repeat == 1 ? (r % tall) / _scale : r;
                const uint32_t same = _repeat == 1 ? (r % tall) % _scale : 0;
                if (r >= tall || same != 0) { // a row made already
                        std::memcpy(row, tile + size_t(r >= tall ? r % tall : r - same) * rowBytes, rowBytes);
                        continue;
                }
                Pixels::fromArgb(_argb + size_t(y) * _width, _width, plain, PixelFormat::Bgr24, _background);
                if (_scale == 1) {
                        std::memcpy(row, plain, size_t(_width) * 3);
                } else {
                        for (uint32_t x = 0; x < _period; ++x) std::memcpy(row + x * 3, plain + (x / _scale) * 3, 3);
                }
                for (uint32_t at = _period; at < _rowWidth; at += _period)
                        std::memcpy(row + size_t(at) * 3, row, size_t(_period) * 3);
        }
        Memory::free(_tile);
        _tile = tile;
        return Error::Ok;
}

void TiledPattern::setBackground(uint32_t rgb) {
        rgb &= 0xffffffu;
        if (_argb == nullptr || _tile == nullptr || rgb == _background) return;
        _background = rgb;
        if (build().isError()) reset(); // no memory: the desktop's color alone
        return;
}

void TiledPattern::reset() {
        Memory::free(_argb);
        Memory::free(_tile);
        _argb = nullptr;
        _tile = nullptr;
        _width = _height = _period = _rowWidth = _rows = 0;
        _repeat = _scale = 1;
        return;
}

size_t TiledPattern::bytes() const {
        return (_tile != nullptr ? size_t(_rowWidth) * 3 * _rows : 0) +
               (_argb != nullptr ? size_t(_width) * _height * 4 : 0);
}

void TiledPattern::make(int x, int y, uint16_t width, uint16_t height, uint8_t *out, size_t stride) const {
        if (_tile == nullptr || width == 0) return;
        const size_t   rowBytes = size_t(_rowWidth) * 3;
        const uint32_t phase = wrap(x, _period);
        for (uint32_t r = 0; r < height; ++r) {
                const uint8_t *row = _tile + size_t(wrap(y + int(r), uint32_t(_rows) * _repeat) / _repeat) * rowBytes;
                uint8_t       *to = out + r * stride;
                // From the phase to the kept row's end, then whole kept rows.
                uint32_t from = phase, left = width;
                while (left != 0) {
                        const uint32_t n = _rowWidth - from < left ? _rowWidth - from : left;
                        std::memcpy(to, row + size_t(from) * 3, size_t(n) * 3);
                        to += size_t(n) * 3;
                        left -= n;
                        from = 0;
                }
        }
        return;
}

bool TiledPattern::tile(Tile &out) const {
        if (_tile == nullptr) return false;
        out = Tile{_tile, size_t(_rowWidth) * 3, _rowWidth, _rows, _repeat, 0, 0};
        return true;
}

BMDS_NAMESPACE_END
