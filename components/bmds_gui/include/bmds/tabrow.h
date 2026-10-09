/**
 * @file      tabrow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A tab widget's row of tabs (`devplan/mediaplayer.md` §6d), pure:
 *        where each tab goes, which one a point is on, and the keys' steps.
 *
 * Each tab is its text and `Pad` either side, on 4-pixel columns, side by
 * side from the row's start. A row too narrow cuts the last ones off.
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace TabRow {

        /** @brief A tab's text from its edges. */
        inline constexpr uint16_t Pad = 8;

        /**
         * @brief Places tabs whose texts are @p textWidths wide from @p start:
         *        their left edges into @p lefts and widths into @p widths (as
         *        many as @p textWidths).
         * @return Where the last one ends.
         */
        uint32_t place(Span<const uint16_t> textWidths, uint16_t start, Span<uint16_t> lefts, Span<uint16_t> widths);

        /** @brief Returns the tab placed at @p x (`place()`'s), or -1. */
        int at(Span<const uint16_t> lefts, Span<const uint16_t> widths, int x);

        /**
         * @brief Returns the tab @p by tabs from @p current of @p count, around
         *        the ends (Ctrl+Tab, Ctrl+PageDown), or 0 if there are none.
         */
        size_t step(size_t current, size_t count, int by);

} // namespace TabRow

BMDS_NAMESPACE_END
