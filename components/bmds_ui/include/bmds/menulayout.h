/**
 * @file      menulayout.h
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
#include <bmds/bitmapfont.h>
#include <bmds/icon.h>
#include <bmds/mnemonic.h>
#include <bmds/rect.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A menu's geometry (`devplan/gui.md` §4b): where each item is, which
 *        one a point is on, the next one the keys go to, and where the menu
 *        goes on the screen.
 *
 * An item is a row: a 16-pixel column for its icon or check mark, its text,
 * its shortcut's text at the right, and an arrow if it opens a submenu. A
 * separator is a short row with a line. The left edges of the texts are on
 * 4-pixel columns (as the painter draws text), and so is the width. The
 * toolkit's `Menu` and the window manager's window menu both use it, and
 * `drawMenu()` draws from it.
 *
 * @par Thread Safety
 * A plain value.
 */
class MenuLayout {
        public:
                /** @brief An item's flags. */
                enum Flag : uint8_t {
                        Separator = 0x01, ///< A line, not an item.
                        Disabled = 0x02,  ///< Shown, not chosen.
                        Checked = 0x04,   ///< A check mark in the icon's column.
                        Submenu = 0x08,   ///< Opens another menu: an arrow at the right.
                };

                /** @brief An item to lay out (the texts are the caller's). */
                struct Item {
                                StringView  text;
                                StringView  shortcut;       ///< Shown at the right ("Ctrl+Q"); may be empty.
                                uint8_t     flags = 0;      ///< `Flag`s.
                                const Icon *icon = nullptr; ///< In the icon column (16 × 16), unless checked.
                                size_t      underline = Mnemonic::None; ///< The text's byte underlined (its key).
                };

                static constexpr size_t   MaxItems = 32;
                static constexpr uint16_t ItemHeight = 20;     ///< A font row and 2 pixels above and below.
                static constexpr uint16_t SeparatorHeight = 8; ///< The line in its middle.
                static constexpr uint16_t Edge = 2;            ///< The bevel around (`Bevel::Style::Window`).
                static constexpr uint16_t InsetY = 3;          ///< Above the first item (the edge included).
                static constexpr uint16_t IconX = 8;           ///< The icon column's left.
                static constexpr uint16_t IconSize = 16;
                static constexpr uint16_t TextX = 32;       ///< The texts' left (4-pixel column).
                static constexpr uint16_t ShortcutGap = 24; ///< Between a text and its shortcut.
                static constexpr uint16_t ArrowWidth = 8;   ///< The submenu arrow (a glyph).
                static constexpr uint16_t RightPad = 12;    ///< After the last column.
                static constexpr uint16_t MinWidth = 96;

                /**
                 * @brief Lays @p items out, texts measured in @p font, at least
                 *        @p minWidth wide (rounded up to a multiple of 4).
                 * @return `Ok`, or `Full` (more than `MaxItems`).
                 */
                Error layout(Span<const Item> items, const BitmapFont &font, uint16_t minWidth = MinWidth);

                /** @brief Returns the items laid out. */
                size_t count() const { return _count; }

                /** @brief Returns the whole menu's width (a multiple of 4). */
                uint16_t width() const { return _width; }

                /** @brief Returns the whole menu's height. */
                uint16_t height() const { return _height; }

                /** @brief Returns item @p i's row, inside the edges (empty past the items). */
                Rect itemRect(size_t i) const;

                /** @brief Returns where item @p i's shortcut text starts (a 4-pixel column). */
                uint16_t shortcutX(size_t i) const;

                /** @brief Returns the arrow's left edge. */
                uint16_t arrowX() const { return static_cast<uint16_t>(_width - RightPad - ArrowWidth); }

                /** @brief Returns item @p i's flags. */
                uint8_t flags(size_t i) const { return i < _count ? _flags[i] : 0; }

                /** @brief Returns true if item @p i can be highlighted and chosen. */
                bool isSelectable(size_t i) const { return i < _count && (_flags[i] & (Separator | Disabled)) == 0; }

                /**
                 * @brief Returns the item at (@p x, @p y) in the menu, or -1 (outside,
                 *        on an edge). Separators and disabled items count.
                 */
                int itemAt(int x, int y) const;

                /**
                 * @brief Returns the next selectable item from @p from (-1: none yet)
                 *        in direction @p step (1 or -1), around the ends, or -1.
                 */
                int next(int from, int step) const;

                /**
                 * @brief Places a menu of @p width × @p height for @p anchor in @p area
                 *        (the screen, or its work area): a context menu's point (an
                 *        empty rectangle) or a button opens it below, left edges
                 *        together, or above when it doesn't fit; a submenu's parent
                 *        item opens it to the right, or the left. Kept inside
                 *        @p area, x on a 4-pixel column.
                 */
                static Rect place(uint16_t width, uint16_t height, const Rect &anchor, const Rect &area, bool submenu);

        private:
                uint16_t _top[MaxItems] = {};
                uint16_t _shortcutX[MaxItems] = {};
                uint8_t  _flags[MaxItems] = {};
                size_t   _count = 0;
                uint16_t _width = 0;
                uint16_t _height = 0;
};

class Painter;

/**
 * @brief Draws a menu laid out by @p layout (its @p items) with @p painter,
 *        whose origin is the menu's top-left corner: the face, a window's
 *        bevel, each item's text, shortcut, check mark and submenu arrow,
 *        etched separators, item @p highlight (-1: none) highlighted. Only
 *        rows the painter's clip shows are drawn. Colors are the painter's
 *        palette's (`face` and its shades, `face-text`, `highlight`,
 *        `highlight-text`, `disabled-text`).
 */
void drawMenu(Painter &painter, const MenuLayout &layout, Span<const MenuLayout::Item> items, int highlight);

BMDS_NAMESPACE_END
