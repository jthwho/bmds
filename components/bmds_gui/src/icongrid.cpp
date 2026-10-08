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
#include <bmds/list.h>

BMDS_NAMESPACE_BEGIN

Error IconGrid::place(Span<const Cell> wanted, uint16_t columns, uint16_t rows, Span<Cell> out) {
        if (columns == 0) columns = 1;
        if (rows == 0) rows = 1;
        const size_t n = wanted.size() < out.size() ? wanted.size() : out.size();
        // The cells taken, a bit each: the grid's, then as many columns past it as all the items need.
        const size_t   cells = (size_t(columns) + (n + rows - 1) / rows) * rows;
        List<uint32_t> taken(MemSpace::Psram);
        Error          err = taken.reserve((cells + 31) / 32);
        if (err.isError()) return err;
        for (size_t w = 0; w < (cells + 31) / 32; ++w) (void)taken.append(0);
        auto index = [rows](const Cell &c) {
                return size_t(c.column) * rows + c.row;
        };
        auto isTaken = [&](size_t i) {
                return (taken[i / 32] & (1u << (i % 32))) != 0;
        };
        auto take = [&](size_t i) {
                taken[i / 32] |= 1u << (i % 32);
        };
        for (size_t i = 0; i < n; ++i) {
                out[i] = Cell();
                const Cell &w = wanted[i];
                if (!w.isValid() || w.column >= columns || w.row >= rows || isTaken(index(w))) continue;
                out[i] = w;
                take(index(w));
        }
        // The rest, in order, in the first free cells (wanted ones placed later count too).
        size_t at = 0;
        for (size_t i = 0; i < n; ++i) {
                if (out[i].isValid()) continue;
                while (isTaken(at)) ++at;
                out[i] = Cell{uint16_t(at / rows), uint16_t(at % rows)};
                take(at++);
        }
        return Error::Ok;
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
