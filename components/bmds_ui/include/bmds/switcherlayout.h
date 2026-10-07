/**
 * @file      switcherlayout.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief Alt+Tab's box (`devplan/gui.md` §4n): its windows' icons in a grid
 *        of cells, the chosen window's title in a field under them.
 *
 * Rectangles are the box's own (its top-left corner at 0, 0). Cells and
 * icons start on 4-pixel columns, as the painter draws pictures, and so does
 * the title's text area.
 *
 * @par Thread Safety
 * A plain value.
 */
class SwitcherLayout {
        public:
                static constexpr uint16_t IconSide = 32;    ///< An icon's.
                static constexpr uint16_t Cell = 48;        ///< A cell's side: the icon, 8 pixels around.
                static constexpr uint16_t Margin = 12;      ///< Inside the box's edge.
                static constexpr uint16_t TitleHeight = 24; ///< The title's field.
                static constexpr uint16_t Gap = 8;          ///< Between the cells and the field.
                static constexpr size_t   MaxColumns = 8;
                static constexpr uint16_t MinWidth = 256;

                /** @brief Lays out @p count cells (one at least), as wide as @p maxWidth allows. */
                void layout(size_t count, uint16_t maxWidth);

                uint16_t width() const { return _width; }
                uint16_t height() const { return _height; }
                size_t   count() const { return _count; }
                size_t   columns() const { return _columns; }

                /** @brief Returns cell @p i's rectangle (empty past the cells). */
                Rect cellRect(size_t i) const;

                /** @brief Returns where cell @p i's icon goes (32 × 32, centred). */
                Rect iconRect(size_t i) const;

                /** @brief Returns the title's field (a sunken bevel around its text). */
                Rect titleRect() const;

                /** @brief Returns the cell under (@p x, @p y), or -1. */
                int cellAt(int x, int y) const;

        private:
                uint16_t _width = 0;
                uint16_t _height = 0;
                uint16_t _left = 0; // the first column's left edge
                size_t   _count = 0;
                size_t   _columns = 1;
};

BMDS_NAMESPACE_END
