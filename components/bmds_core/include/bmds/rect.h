/**
 * @file      rect.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A rectangle of pixels: its top-left corner and size.
 *
 * The right and bottom edges are exclusive (`right()` is the first column
 * past it). A rectangle of no width or height is empty.
 *
 * @par Thread Safety
 * A plain value.
 */
struct Rect {
                uint16_t x = 0;
                uint16_t y = 0;
                uint16_t width = 0;
                uint16_t height = 0;

                /** @brief Returns the first column past the right edge. */
                constexpr uint32_t right() const { return uint32_t(x) + width; }

                /** @brief Returns the first row past the bottom edge. */
                constexpr uint32_t bottom() const { return uint32_t(y) + height; }

                /** @brief Returns true if it covers no pixel. */
                constexpr bool isEmpty() const { return width == 0 || height == 0; }

                /** @brief Returns the pixel count. */
                constexpr uint32_t area() const { return uint32_t(width) * height; }

                /** @brief Returns true if they share a pixel. */
                constexpr bool intersects(const Rect &other) const {
                        return !isEmpty() && !other.isEmpty() && x < other.right() && other.x < right() &&
                               y < other.bottom() && other.y < bottom();
                }

                /** @brief Returns true if @p other is inside it (an empty one always is). */
                constexpr bool contains(const Rect &other) const {
                        return other.isEmpty() ||
                               (other.x >= x && other.right() <= right() && other.y >= y && other.bottom() <= bottom());
                }

                /** @brief Returns the shared part (empty if none). */
                constexpr Rect intersected(const Rect &other) const {
                        if (!intersects(other)) return Rect();
                        const uint32_t left = x > other.x ? x : other.x;
                        const uint32_t top = y > other.y ? y : other.y;
                        const uint32_t r = right() < other.right() ? right() : other.right();
                        const uint32_t b = bottom() < other.bottom() ? bottom() : other.bottom();
                        return Rect{uint16_t(left), uint16_t(top), uint16_t(r - left), uint16_t(b - top)};
                }

                /** @brief Returns the smallest rectangle holding both (an empty one adds nothing). */
                constexpr Rect united(const Rect &other) const {
                        if (other.isEmpty()) return *this;
                        if (isEmpty()) return other;
                        const uint32_t left = x < other.x ? x : other.x;
                        const uint32_t top = y < other.y ? y : other.y;
                        const uint32_t r = right() > other.right() ? right() : other.right();
                        const uint32_t b = bottom() > other.bottom() ? bottom() : other.bottom();
                        return Rect{uint16_t(left), uint16_t(top), uint16_t(r - left), uint16_t(b - top)};
                }

                /**
                 * @brief Returns it widened so its left and right edges are
                 *        multiples of @p multiple (a power of two): what a
                 *        DMA that moves whole words needs.
                 */
                constexpr Rect alignedX(uint16_t multiple) const {
                        const uint32_t mask = multiple - 1u;
                        const uint32_t left = x & ~mask;
                        const uint32_t r = (right() + mask) & ~mask;
                        return Rect{uint16_t(left), y, uint16_t(r - left), height};
                }

                constexpr bool operator==(const Rect &other) const = default;
};

BMDS_NAMESPACE_END
