/**
 * @file      furniturelayout.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A menu bar's titles, pure (`devplan/filebrowser.md` §4e): side by
 *        side from the left, each its text and padding, on 4-pixel columns.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct MenuBarLayout {
                static constexpr uint16_t Pad = 8; ///< Each side of a title's text.

                /**
                 * @brief Places titles whose texts are @p textWidths wide from @p x,
                 *        @p height high at @p y, into @p out (as many as there are).
                 */
                static void place(Span<const uint16_t> textWidths, uint16_t x, uint16_t y, uint16_t height,
                                  Span<Rect> out);

                /** @brief Returns the title at (@p x, @p y), or -1. */
                static int titleAt(Span<const Rect> titles, int x, int y);

                /** @brief Returns the title @p step from @p from among @p count, around the ends. */
                static size_t neighbour(size_t from, int step, size_t count);
};

/**
 * @brief Where a splitter's handle goes, pure: each side at least its least.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct SplitterLayout {
                static constexpr uint16_t Handle = 4; ///< The handle's thickness.

                /**
                 * @brief Returns the first side's size for @p wanted along @p length:
                 *        a multiple of @p unit, no less than @p firstLeast and leaving
                 *        the second side @p secondLeast (when both can't be, the
                 *        first side's least wins), within the length.
                 */
                static uint16_t firstSize(int wanted, uint16_t length, uint16_t firstLeast, uint16_t secondLeast,
                                          uint16_t unit);
};

/**
 * @brief A progress bar's blocks, pure (as Windows 95's: blocks with gaps).
 *
 * @par Thread Safety
 * Reentrant.
 */
struct ProgressLayout {
                static constexpr uint16_t Block = 8; ///< A block's width.
                static constexpr uint16_t Gap = 4;   ///< Between blocks (blocks on 4-pixel columns).

                /** @brief Returns the blocks that fit @p width. */
                static uint16_t capacity(uint16_t width) {
                        return static_cast<uint16_t>((width + Gap) / (Block + Gap));
                }

                /**
                 * @brief Returns the blocks showing @p value of @p maximum in @p width:
                 *        rounded down, all of them only at the maximum.
                 */
                static uint16_t blocks(uint64_t value, uint64_t maximum, uint16_t width);
};

/**
 * @brief A dialog's buttons, pure: a row at the right, each its text and
 *        padding or a least width, on 4-pixel columns.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct ButtonRow {
                static constexpr uint16_t MinWidth = 76; ///< A button's least width.
                static constexpr uint16_t Spacing = 8;   ///< Between buttons.

                /** @brief Returns the width of a button whose text is @p textWidth wide. */
                static uint16_t buttonWidth(uint16_t textWidth);

                /** @brief Returns the row's width: the buttons and the spacing between. */
                static uint16_t width(Span<const uint16_t> textWidths);

                /**
                 * @brief Places buttons (texts @p textWidths wide) in a row ending
                 *        at @p right, @p height high at @p y, into @p out.
                 */
                static void place(Span<const uint16_t> textWidths, uint16_t right, uint16_t y, uint16_t height,
                                  Span<Rect> out);
};

BMDS_NAMESPACE_END
