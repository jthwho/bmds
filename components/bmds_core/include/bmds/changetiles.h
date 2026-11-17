/**
 * @file      changetiles.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/rect.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What changed on a picture, as a bitmap of `TileWidth` × `TileHeight`
 *        tiles (`devplan/vnc.md` §5b): a screen watcher's.
 *
 * Fixed size, no allocation: up to `MaxColumns` × `MaxRows` tiles (a
 * 2048 × 1088 picture). The tiles are those a `Surface` marks as it draws
 * (32 × 16, a whole number of a 4:2:0 JPEG's MCUs), so a surface's own
 * bitmap ORs in a row of words at a time. `takeRects()` gives the marked
 * tiles back as few rectangles, each tiles' worth of pixels clipped to the
 * picture.
 *
 * @par Thread Safety
 * Not thread-safe: its owner locks.
 */
class ChangeTiles {
        public:
                /** @brief A tile's size in pixels (`Surface`'s). */
                static constexpr uint16_t TileWidth = 32;
                static constexpr uint16_t TileHeight = 16;

                /** @brief Tiles across and down at most. */
                static constexpr uint16_t MaxColumns = 64;
                static constexpr uint16_t MaxRows = 68;

                /** @brief Words a row of tiles. */
                static constexpr uint16_t RowWords = MaxColumns / 32;

                constexpr ChangeTiles() = default;

                /**
                 * @brief Sets the picture's size and marks every tile (a new picture is all new).
                 * @return False if it is bigger than `MaxColumns` × `MaxRows` tiles (nothing changed).
                 */
                bool resize(uint16_t width, uint16_t height);

                /** @brief Returns the picture's width. */
                uint16_t width() const { return _width; }

                /** @brief Returns the picture's height. */
                uint16_t height() const { return _height; }

                /** @brief Returns true if nothing is marked. */
                bool isEmpty() const;

                /** @brief Marks every tile. */
                void markAll();

                /** @brief Marks the tiles @p rect touches (clipped to the picture). */
                void mark(const Rect &rect);

                /**
                 * @brief ORs in another bitmap of the same tiles: @p rows rows of @p rowWords words
                 *        each, bit c of row r (word c / 32) the tile at column c (a `Surface`'s).
                 *        Rows and columns past this picture's are ignored.
                 */
                void orIn(const uint32_t *tiles, uint16_t rowWords, uint16_t rows);

                /** @brief Clears every tile. */
                void clear();

                /**
                 * @brief Takes marked tiles as rectangles into @p out: each a run of tiles across,
                 *        grown down while the rows below have the same run, so they never overlap.
                 *        The tiles taken are cleared; it stops when @p out is full or the pixels reach
                 *        @p maxPixels: a rectangle too big for what is left gives its top rows of
                 *        tiles that fit (the rest stays marked), so a budget of a tile row of the
                 *        picture's width always makes progress. A take that stops early is followed
                 *        by one that starts where it stopped (wrapping round), so a picture that
                 *        changes all over every frame is taken in turn, not its top again and again.
                 * @return The rectangles written.
                 */
                size_t takeRects(Span<Rect> out, uint32_t maxPixels);

        private:
                bool isSet(uint16_t column, uint16_t row) const {
                        return (_bits[row][column / 32] >> (column % 32)) & 1u;
                }
                void clearTile(uint16_t column, uint16_t row) {
                        _bits[row][column / 32] &= ~(1u << (column % 32));
                        return;
                }
                Rect pixels(uint16_t c0, uint16_t c1, uint16_t r0, uint16_t r1) const; // [c0, c1) × [r0, r1)

                uint32_t _bits[MaxRows][RowWords] = {};
                uint16_t _width = 0;
                uint16_t _height = 0;
                uint16_t _columns = 0;
                uint16_t _rows = 0;
                uint16_t _nextRow = 0; // where the next take starts: where the last stopped early
};

BMDS_NAMESPACE_END
