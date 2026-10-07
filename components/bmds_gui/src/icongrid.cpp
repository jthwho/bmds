/**
 * @file      icongrid.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Icons in a grid's cells (devplan/gui.md §4m).
 */

#include <bmds/icongrid.h>

BMDS_NAMESPACE_BEGIN

void IconGrid::place(Span<const Cell> wanted, uint16_t columns, uint16_t rows, Span<Cell> out) {
        if (columns == 0) columns = 1;
        if (rows == 0) rows = 1;
        size_t n = wanted.size() < out.size() ? wanted.size() : out.size();
        if (n > MaxItems) n = MaxItems;
        // Taken: an item's cell is checked against the earlier ones' (few items: a walk).
        auto taken = [&](const Cell &c, size_t before) {
                for (size_t k = 0; k < before; ++k)
                        if (out[k] == c) return true;
                return false;
        };
        bool placed[MaxItems] = {};
        for (size_t i = 0; i < n; ++i) {
                out[i] = Cell();
                const Cell &w = wanted[i];
                if (w.isValid() && w.column < columns && w.row < rows && !taken(w, i)) {
                        out[i] = w;
                        placed[i] = true;
                }
        }
        // The rest, in order, in the first free cells (wanted ones placed later count too).
        size_t at = 0;
        for (size_t i = 0; i < n; ++i) {
                if (placed[i]) continue;
                for (;; ++at) {
                        const Cell c{uint16_t(at / rows), uint16_t(at % rows)};
                        if (taken(c, n)) continue;
                        out[i] = c;
                        ++at;
                        break;
                }
        }
        return;
}

IconGrid::Cell IconGrid::cellAt(int x, int y, uint16_t width, uint16_t height, uint16_t columns, uint16_t rows) {
        auto index = [](int v, uint16_t size, uint16_t count) {
                if (v < 0 || size == 0 || count == 0) return uint16_t(0);
                const int i = v / size;
                return uint16_t(i < count ? i : count - 1);
        };
        return Cell{index(x, width, columns), index(y, height, rows)};
}

BMDS_NAMESPACE_END
