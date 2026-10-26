/**
 * @file      backdrop.h
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
#include <bmds/rect.h>

BMDS_NAMESPACE_BEGIN

class Painter;
struct Icon;

/**
 * @brief Pixels a view is drawn on instead of its background color: the desktop's pattern
 *        (`devplan/wallpaper.md` §4d, §4e). Its owner makes them; `paint()` and `paintIcon()`
 *        draw them through a painter, a strip at a time.
 *
 * @par Thread Safety
 * As its owner's: the application's lock.
 */
class Backdrop {
        public:
                /**
                 * @brief Makes @p width × @p height B, G, R pixels of what is at (@p x, @p y) (the
                 *        window's coordinates) into @p out, @p stride bytes a row.
                 * @hotpath
                 */
                virtual void make(int x, int y, uint16_t width, uint16_t height, uint8_t *out, size_t stride) const = 0;

                /**
                 * @brief Draws @p rect (the painter's coordinates, the window's) with it, widened to
                 *        4-pixel columns (the window at a column).
                 * @return The painter's first error.
                 * @blocking The copies.
                 */
                Error paint(Painter &painter, const Rect &rect) const;

                /**
                 * @brief Draws @p icon at (@p x, @p y) (x on a 4-pixel column, as `Painter::image()`),
                 *        its alpha blended over this.
                 * @return The painter's first error.
                 * @blocking The copies.
                 */
                Error paintIcon(Painter &painter, int x, int y, const Icon &icon) const;

        protected:
                ~Backdrop() = default;
};

BMDS_NAMESPACE_END
