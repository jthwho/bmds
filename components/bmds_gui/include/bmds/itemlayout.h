/**
 * @file      itemlayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/icongrid.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/** @brief A rectangle of a view's content (32-bit: content outgrows the screen). */
struct ContentRect {
                uint32_t x = 0;
                uint32_t y = 0;
                uint32_t width = 0;
                uint32_t height = 0;

                /** @brief Returns true if it and @p o share a pixel. */
                constexpr bool intersects(const ContentRect &o) const {
                        return width != 0 && height != 0 && o.width != 0 && o.height != 0 && x < o.x + o.width &&
                               o.x < x + width && y < o.y + o.height && o.y < y + height;
                }

                /** @brief Returns true if (@p px, @p py) is in it. */
                constexpr bool contains(uint32_t px, uint32_t py) const {
                        return px >= x && py >= y && px < x + width && py < y + height;
                }
};

/**
 * @brief Where an icon view's items go, pure (`devplan/filebrowser.md` §4g):
 *        flowing cells (rows left to right, as many columns as fit), or the
 *        desktop's grid cells (`IconGrid`).
 *
 * Content coordinates are 32-bit (a long folder is taller than 65535 pixels).
 *
 * @par Thread Safety
 * Reentrant.
 */
struct IconLayout {
                static constexpr uint16_t CellWidth = 80; ///< An icon, its name in two lines, room around.
                static constexpr uint16_t CellHeight = 76;

                /** @brief Returns the columns that fit @p width (one at least). */
                static uint16_t columnsFor(uint16_t width) {
                        return width / CellWidth > 0 ? static_cast<uint16_t>(width / CellWidth) : 1;
                }

                /** @brief Returns flowing cell @p row's left edge. */
                static uint32_t flowX(size_t row, uint16_t columns) { return uint32_t(row % columns) * CellWidth; }

                /** @brief Returns flowing cell @p row's top edge. */
                static uint32_t flowY(size_t row, uint16_t columns) { return uint32_t(row / columns) * CellHeight; }

                /** @brief Returns the height @p count flowing cells take. */
                static uint32_t flowHeight(size_t count, uint16_t columns) {
                        return uint32_t((count + columns - 1) / columns) * CellHeight;
                }

                /** @brief Returns the flowing cell at (@p x, @p y), or -1 (none there). */
                static int flowRowAt(uint32_t x, uint32_t y, uint16_t columns, size_t count);

                /**
                 * @brief Returns the row an arrow (@p dx, @p dy: one of them ±1) goes to
                 *        from @p row among @p count flowing cells: the next cell across;
                 *        a line up or down (the last item from a line above a short
                 *        last line). @p row itself if none is that way.
                 */
                static size_t flowNeighbour(size_t row, int dx, int dy, uint16_t columns, size_t count);

                /**
                 * @brief Returns the item nearest @p from that way on a grid (@p dx,
                 *        @p dy): the nearest along it, then across it; @p from if none.
                 */
                static int gridNeighbour(Span<const IconGrid::Cell> cells, int from, int dx, int dy);

                /** @brief Returns true if @p cell is in the rectangle of cells from @p a to @p b (a grid's Shift range). */
                static constexpr bool inGridRange(IconGrid::Cell cell, IconGrid::Cell a, IconGrid::Cell b) {
                        const uint16_t c0 = a.column < b.column ? a.column : b.column;
                        const uint16_t c1 = a.column < b.column ? b.column : a.column;
                        const uint16_t r0 = a.row < b.row ? a.row : b.row, r1 = a.row < b.row ? b.row : a.row;
                        return cell.column >= c0 && cell.column <= c1 && cell.row >= r0 && cell.row <= r1;
                }
};

/**
 * @brief A details view's columns, pure (`devplan/filebrowser.md` §4g).
 *
 * @par Thread Safety
 * Reentrant.
 */
struct HeaderLayout {
                static constexpr uint16_t MinWidth = 24; ///< A column's least width.
                static constexpr uint16_t EdgeGrab = 3;  ///< How near an edge a press resizes.

                /** @brief Returns the columns' total width. */
                static uint32_t total(Span<const uint16_t> widths);

                /** @brief Returns the column at @p x (from the first's left), or -1. */
                static int columnAt(Span<const uint16_t> widths, int x);

                /** @brief Returns the column whose right edge is within `EdgeGrab` of @p x, or -1. */
                static int edgeAt(Span<const uint16_t> widths, int x);

                /** @brief Returns a column @p start wide dragged by @p dx: on 4-pixel columns, `MinWidth` at least. */
                static uint16_t resized(uint16_t start, int dx);
};

BMDS_NAMESPACE_END
