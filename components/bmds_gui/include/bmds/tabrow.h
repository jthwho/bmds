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
 * @brief A tab widget's row of tabs (`devplan/mediaplayer.md` §6d, `devplan/tabs.md`), pure:
 *        where each tab and its badge and close button go, the row scrolled when it is too
 *        narrow, what a point is on, and the keys' steps.
 *
 * Each tab is its text, its badge and its close button with `Pad` either side, on 4-pixel
 * columns, side by side from the row's start. A row too narrow scrolls (`bar()`): the tabs in a
 * view at its left, three buttons past it; the scroll is a column multiple, so text stays on
 * its columns. (`place()` and `at()` are the row before it scrolled, until `TabWidget` moves
 * to the rest.)
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace TabRow {

        /** @brief A tab's text from its edges. */
        inline constexpr uint16_t Pad = 8;

        /** @brief A badge's pill (devplan/tabs.md §2): from the title, and its text from its edges. */
        inline constexpr uint16_t BadgeGap = 4;
        inline constexpr uint16_t BadgePad = 4;

        /** @brief A close button: its square, from what is before it, and from the tab's right edge. */
        inline constexpr uint16_t CloseSize = 12;
        inline constexpr uint16_t CloseGap = 4;
        inline constexpr uint16_t CloseMargin = 4;

        /** @brief Each of a scrolling row's three buttons (back, forward, the list of tabs). */
        inline constexpr uint16_t ButtonWidth = 16;

        /** @brief A tab's parts, from its left edge: its width, and where its badge and close button go. */
        struct Parts {
                        uint16_t width = 0;
                        uint16_t badge = 0;      ///< The pill's left (0: no badge).
                        uint16_t badgeWidth = 0; ///< The pill's width.
                        uint16_t close = 0;      ///< The close button's left (0: none).
        };

        /**
         * @brief Lays out a tab: its title (@p titleWidth wide) at `Pad`, then a badge whose text is
         *        @p badgeWidth wide (0: none), then a close button if @p closable, the width up to a
         *        column (4 pixels). With neither, it is `place()`'s width.
         */
        Parts parts(uint16_t titleWidth, uint16_t badgeWidth, bool closable);

        /**
         * @brief Places tabs @p widths wide side by side from 0 (the row's own measure, before it
         *        scrolls): their left edges into @p lefts.
         * @return Their width together.
         */
        uint32_t placeAll(Span<const uint16_t> widths, Span<uint32_t> lefts);

        /** @brief How a row shares its width: the tabs' view, and the buttons past it if they scroll. */
        struct Bar {
                        uint32_t view = 0;        ///< The tabs' part, from the row's left (a column multiple).
                        bool     buttons = false; ///< Too narrow: the three buttons at the view's right.
        };

        /** @brief Shares a row @p rowWidth wide by tabs @p content wide together. */
        Bar bar(uint32_t content, uint32_t rowWidth);

        /** @brief Returns how far the row scrolls at the most (0 when it fits). */
        uint32_t maxScroll(uint32_t content, const Bar &bar);

        /**
         * @brief Returns @p scroll changed as little as shows a tab at @p left, @p width wide (its
         *        left edge, if it is wider than the view), within `maxScroll()`.
         */
        uint32_t scrollToShow(uint32_t scroll, uint32_t left, uint16_t width, uint32_t content, const Bar &bar);

        /**
         * @brief Returns @p scroll moved @p by tabs (the buttons, the wheel): to the next tab's left
         *        edge past it, or the last one before it; within `maxScroll()`.
         */
        uint32_t scrollBy(uint32_t scroll, Span<const uint32_t> lefts, uint32_t content, const Bar &bar, int by);

        /** @brief What a point on the row is on. */
        struct Hit {
                        enum class Kind : uint8_t {
                                None,
                                Tab,
                                Close, ///< A tab's close button.
                                Back,
                                Forward,
                                List,
                        };
                        Kind kind = Kind::None;
                        int  tab = -1; ///< `Tab`, `Close`: which.
        };

        /**
         * @brief Returns what @p x (from the row's left) is on, the row scrolled @p scroll: a tab
         *        (@p lefts, @p widths; @p closes their close buttons' lefts as `Parts::close`), its
         *        close button, or a button.
         */
        Hit hit(Span<const uint32_t> lefts, Span<const uint16_t> widths, Span<const uint16_t> closes, uint32_t scroll,
                const Bar &bar, int x);

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
