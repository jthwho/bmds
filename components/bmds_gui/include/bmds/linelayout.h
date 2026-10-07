/**
 * @file      linelayout.h
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
 * @brief Shares a line's length among items (`devplan/gui.md` §4b): the
 *        arithmetic of `BoxLayout`, pure.
 *
 * Each item gets its hint (rounded up to the unit, no more than its most).
 * Room left over goes to the items that stretch, in proportion to their
 * stretch factors, none past its most; room missing comes first from the items
 * that stretch, then from the last items. Every item starts on a multiple of
 * the unit (4: text starts on a 4-pixel column), given spacing and hints in
 * units.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct LineLayout {
                /** @brief An item to place. */
                struct Item {
                                uint16_t hint = 0;          ///< The size it wants.
                                uint16_t most = UINT16_MAX; ///< The size it may grow to.
                                uint8_t  stretch = 0;       ///< Its share of room left over; 0: none.
                };

                /**
                 * @brief Places @p items along @p length pixels, @p spacing apart:
                 *        each one's start in @p position and size in @p size (as
                 *        many as there are items). A unit of 0 counts as 1.
                 */
                static void arrange(Span<const Item> items, uint16_t length, uint16_t spacing, uint16_t unit,
                                    Span<uint16_t> position, Span<uint16_t> size);
};

BMDS_NAMESPACE_END
