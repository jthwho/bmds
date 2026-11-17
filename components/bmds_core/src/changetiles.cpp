/**
 * @file      changetiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/changetiles.h>

BMDS_NAMESPACE_BEGIN

bool ChangeTiles::resize(uint16_t width, uint16_t height) {
        const uint32_t columns = (uint32_t(width) + TileWidth - 1) / TileWidth;
        const uint32_t rows = (uint32_t(height) + TileHeight - 1) / TileHeight;
        if (columns > MaxColumns || rows > MaxRows) return false;
        _width = width;
        _height = height;
        _columns = uint16_t(columns);
        _rows = uint16_t(rows);
        _nextRow = 0;
        markAll();
        return true;
}

bool ChangeTiles::isEmpty() const {
        for (uint16_t r = 0; r < _rows; ++r)
                for (uint16_t w = 0; w < RowWords; ++w)
                        if (_bits[r][w] != 0) return false;
        return true;
}

void ChangeTiles::clear() {
        std::memset(_bits, 0, sizeof(_bits));
        return;
}

void ChangeTiles::markAll() {
        clear();
        for (uint16_t r = 0; r < _rows; ++r)
                for (uint16_t c = 0; c < _columns; ++c) _bits[r][c / 32] |= 1u << (c % 32);
        return;
}

void ChangeTiles::mark(const Rect &rect) {
        if (rect.isEmpty() || rect.x >= _width || rect.y >= _height) return;
        const uint32_t right = rect.right() < _width ? rect.right() : _width;
        const uint32_t bottom = rect.bottom() < _height ? rect.bottom() : _height;
        const uint32_t c0 = rect.x / TileWidth, c1 = (right - 1) / TileWidth;
        const uint32_t r0 = rect.y / TileHeight, r1 = (bottom - 1) / TileHeight;
        for (uint32_t r = r0; r <= r1; ++r)
                for (uint32_t c = c0; c <= c1; ++c) _bits[r][c / 32] |= 1u << (c % 32);
        return;
}

void ChangeTiles::orIn(const uint32_t *tiles, uint16_t rowWords, uint16_t rows) {
        if (tiles == nullptr) return;
        const uint16_t n = rows < _rows ? rows : _rows;
        const uint16_t words = rowWords < RowWords ? rowWords : RowWords;
        for (uint16_t r = 0; r < n; ++r) {
                for (uint16_t w = 0; w < words; ++w) {
                        uint32_t bits = tiles[size_t(r) * rowWords + w];
                        // Columns past the picture's (a wider source) dropped.
                        const uint32_t first = uint32_t(w) * 32;
                        if (first >= _columns) {
                                bits = 0;
                        } else if (_columns - first < 32) {
                                bits &= (1u << (_columns - first)) - 1u;
                        }
                        _bits[r][w] |= bits;
                }
        }
        return;
}

bool ChangeTiles::touches(const Rect &rect) const {
        if (rect.isEmpty() || rect.x >= _width || rect.y >= _height) return false;
        const uint32_t right = rect.right() < _width ? rect.right() : _width;
        const uint32_t bottom = rect.bottom() < _height ? rect.bottom() : _height;
        const uint32_t c0 = rect.x / TileWidth, c1 = (right - 1) / TileWidth;
        const uint32_t r0 = rect.y / TileHeight, r1 = (bottom - 1) / TileHeight;
        for (uint32_t r = r0; r <= r1; ++r)
                for (uint32_t c = c0; c <= c1; ++c)
                        if (isSet(uint16_t(c), uint16_t(r))) return true;
        return false;
}

void ChangeTiles::clearCovered(const Rect &rect) {
        if (rect.isEmpty() || rect.x >= _width || rect.y >= _height) return;
        const uint32_t right = rect.right() < _width ? rect.right() : _width;
        const uint32_t bottom = rect.bottom() < _height ? rect.bottom() : _height;
        // The first tile starting inside, to the last ending inside (or at the picture's edge).
        const uint32_t c0 = (uint32_t(rect.x) + TileWidth - 1) / TileWidth;
        const uint32_t r0 = (uint32_t(rect.y) + TileHeight - 1) / TileHeight;
        const uint32_t c1 = right == _width ? _columns : right / TileWidth; // exclusive
        const uint32_t r1 = bottom == _height ? _rows : bottom / TileHeight;
        for (uint32_t r = r0; r < r1; ++r)
                for (uint32_t c = c0; c < c1; ++c) clearTile(uint16_t(c), uint16_t(r));
        return;
}

Rect ChangeTiles::pixels(uint16_t c0, uint16_t c1, uint16_t r0, uint16_t r1) const {
        const uint32_t x = uint32_t(c0) * TileWidth, y = uint32_t(r0) * TileHeight;
        const uint32_t right = uint32_t(c1) * TileWidth < _width ? uint32_t(c1) * TileWidth : _width;
        const uint32_t bottom = uint32_t(r1) * TileHeight < _height ? uint32_t(r1) * TileHeight : _height;
        return Rect{uint16_t(x), uint16_t(y), uint16_t(right - x), uint16_t(bottom - y)};
}

size_t ChangeTiles::takeRects(Span<Rect> out, uint32_t maxPixels) {
        size_t   count = 0;
        uint32_t used = 0;
        // From where the last take stopped, round to it. A rectangle grows down to the bottom at
        // most (rows above the start were taken already this time, or are left for the next).
        const uint16_t start = _nextRow < _rows ? _nextRow : 0;
        for (uint16_t step = 0; step < _rows; ++step) {
                const uint16_t r = uint16_t((start + step) % _rows);
                for (uint16_t c = 0; c < _columns;) {
                        if (!isSet(c, r)) {
                                ++c;
                                continue;
                        }
                        // A run across, then down while the next row has it all.
                        const uint16_t c0 = c;
                        while (c < _columns && isSet(c, r)) ++c;
                        uint16_t r1 = uint16_t(r + 1);
                        for (; r1 < _rows; ++r1) {
                                bool whole = true;
                                for (uint16_t k = c0; k < c && whole; ++k) whole = isSet(k, r1);
                                if (!whole) break;
                        }
                        Rect rect = pixels(c0, c, r, r1);
                        if (count == out.size()) {
                                _nextRow = r;
                                return count;
                        }
                        if (used + rect.area() > maxPixels) {
                                // Too big for what is left: its top rows of tiles that fit, if any (a
                                // rectangle bigger than the whole budget is taken a band at a time).
                                const uint32_t rowPixels = uint32_t(pixels(c0, c, r, uint16_t(r + 1)).width) * TileHeight;
                                const uint32_t rows = (maxPixels - used) / rowPixels;
                                if (rows == 0) {
                                        _nextRow = r;
                                        return count;
                                }
                                r1 = uint16_t(r + rows);
                                rect = pixels(c0, c, r, r1);
                                out[count++] = rect;
                                for (uint16_t row = r; row < r1; ++row)
                                        for (uint16_t k = c0; k < c; ++k) clearTile(k, row);
                                _nextRow = r1 < _rows ? r1 : 0;
                                return count;
                        }
                        out[count++] = rect;
                        used += rect.area();
                        for (uint16_t row = r; row < r1; ++row)
                                for (uint16_t k = c0; k < c; ++k) clearTile(k, row);
                }
        }
        return count;
}

BMDS_NAMESPACE_END
