/**
 * @file      region.h
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
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An area of the screen as a few disjoint rectangles: the damage a
 *        window manager collects between composes (`devplan/windows.md`).
 *
 * Adding keeps the rectangles disjoint and merges neighbors that make a
 * rectangle together. Past `MaxRects` it collapses into the bounding box: a
 * region may grow beyond what was added, never lose any of it.
 *
 * @par Thread Safety
 * A plain value.
 */
class Region {
        public:
                /** @brief Most rectangles held before collapsing into the bounds. */
                static constexpr size_t MaxRects = 32;

                /** @brief Returns true if it covers nothing. */
                bool isEmpty() const { return _count == 0; }

                /** @brief Returns the rectangles: disjoint, none empty. */
                Span<const Rect> rects() const { return Span<const Rect>(_rects, _count); }

                /** @brief Returns the smallest rectangle holding it. */
                Rect bounds() const;

                /** @brief Returns the pixels covered. */
                uint32_t area() const;

                /** @brief Adds @p rect. */
                void add(const Rect &rect);

                /** @brief Adds every rectangle of @p other. */
                void add(const Region &other);

                /** @brief Makes it empty. */
                void clear() {
                        _count = 0;
                        return;
                }

        private:
                void collapse(const Rect &extra);
                void merge();

                Rect    _rects[MaxRects];
                uint8_t _count = 0;
};

/**
 * @brief Writes @p a minus @p b into @p out as up to four disjoint
 *        rectangles (bands above and below, pieces left and right).
 * @return The count written.
 */
size_t subtract(const Rect &a, const Rect &b, Rect (&out)[4]);

/** @brief One copy of a compose: a rectangle of the screen from a layer. */
struct ComposePiece {
                uint16_t layer = 0; ///< Index into the layers given.
                Rect     rect;      ///< Screen coordinates, inside the layer.
};

/**
 * @brief Plans a compose: which part of each layer shows in @p damage.
 *
 * @p layers are the opaque layers' screen rectangles, the top one first
 * (the desktop last, covering the screen). Each damaged pixel comes from
 * the topmost layer covering it; pixels no layer covers are left out. The
 * pieces are disjoint, so nothing is drawn twice. Horizontal edges are
 * only the inputs', so inputs on even columns give even pieces.
 *
 * @return The pieces written to @p out, or `Full` (more pieces than @p out
 *         holds, or than the plan's working space: the caller paints the
 *         damage's bounds bottom to top instead).
 */
Result<size_t> planCompose(Span<const Rect> layers, const Region &damage, Span<ComposePiece> out);

BMDS_NAMESPACE_END
