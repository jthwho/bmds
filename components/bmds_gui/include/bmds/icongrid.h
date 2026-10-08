/**
 * @file      icongrid.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Icons in the cells of a grid (`devplan/gui.md` §4m): the arithmetic
 *        of the desktop's icons, pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct IconGrid {
                /** @brief A cell: its column and row from the grid's top-left one. */
                struct Cell {
                                uint16_t column = None;
                                uint16_t row = None;

                                constexpr bool isValid() const { return column != None && row != None; }
                                constexpr bool operator==(const Cell &other) const = default;
                };

                /** @brief No cell (none wanted). */
                static constexpr uint16_t None = UINT16_MAX;

                /**
                 * @brief Places @p wanted.size() items in a grid of @p columns × @p rows
                 *        cells, in @p out: each in the cell it wants if that is in the
                 *        grid and no earlier item took it, the rest in the first free
                 *        cells down, then across. Past the grid's cells, items go in
                 *        further columns (off it). The cells taken are a PSRAM bitmap.
                 * @return `Ok` or `NoMem`.
                 */
                static Error place(Span<const Cell> wanted, uint16_t columns, uint16_t rows, Span<Cell> out);

                /**
                 * @brief Returns the cell under (@p x, @p y) of a grid of @p columns ×
                 *        @p rows cells of @p width × @p height pixels from (0, 0), the
                 *        nearest one if outside it.
                 */
                static Cell cellAt(int x, int y, uint16_t width, uint16_t height, uint16_t columns, uint16_t rows);
};

BMDS_NAMESPACE_END
