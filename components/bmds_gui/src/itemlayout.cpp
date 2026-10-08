/**
 * @file      itemlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Where an icon view's items and a details view's columns go (devplan/filebrowser.md §4g).
 */

#include <bmds/itemlayout.h>

BMDS_NAMESPACE_BEGIN

int IconLayout::flowRowAt(uint32_t x, uint32_t y, uint16_t columns, size_t count) {
        if (columns == 0) return -1;
        const uint32_t column = x / CellWidth;
        if (column >= columns) return -1;
        const uint64_t row = uint64_t(y / CellHeight) * columns + column;
        return row < count ? int(row) : -1;
}

size_t IconLayout::flowNeighbour(size_t row, int dx, int dy, uint16_t columns, size_t count) {
        if (count == 0 || columns == 0) return row;
        if (dx < 0) return row > 0 ? row - 1 : row;
        if (dx > 0) return row + 1 < count ? row + 1 : row;
        if (dy < 0) return row >= columns ? row - columns : row;
        if (dy > 0) {
                if (row + columns < count) return row + columns;
                // A short last line below: its last item.
                return row / columns < (count - 1) / columns ? count - 1 : row;
        }
        return row;
}

int IconLayout::gridNeighbour(Span<const IconGrid::Cell> cells, int from, int dx, int dy) {
        if (from < 0 || size_t(from) >= cells.size()) return from;
        const IconGrid::Cell at = cells[size_t(from)];
        int                  best = from;
        int                  bestScore = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
                const int x = int(cells[i].column) - int(at.column);
                const int y = int(cells[i].row) - int(at.row);
                const int along = x * dx + y * dy;
                const int across = dx != 0 ? y : x;
                if (along <= 0) continue;
                const int score = along * along + 4 * across * across;
                if (best == from || score < bestScore) {
                        best = int(i);
                        bestScore = score;
                }
        }
        return best;
}

uint32_t HeaderLayout::total(Span<const uint16_t> widths) {
        uint32_t t = 0;
        for (const uint16_t w : widths) t += w;
        return t;
}

int HeaderLayout::columnAt(Span<const uint16_t> widths, int x) {
        if (x < 0) return -1;
        int left = 0;
        for (size_t i = 0; i < widths.size(); ++i) {
                if (x < left + widths[i]) return int(i);
                left += widths[i];
        }
        return -1;
}

int HeaderLayout::edgeAt(Span<const uint16_t> widths, int x) {
        int right = 0;
        for (size_t i = 0; i < widths.size(); ++i) {
                right += widths[i];
                if (x >= right - EdgeGrab && x <= right + EdgeGrab) return int(i);
        }
        return -1;
}

uint16_t HeaderLayout::resized(uint16_t start, int dx) {
        int w = int(start) + dx;
        w = (w + 2) & ~3; // the nearest column
        if (w < MinWidth) w = MinWidth;
        if (w > 4096) w = 4096;
        return static_cast<uint16_t>(w);
}

BMDS_NAMESPACE_END
