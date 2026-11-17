/**
 * @file      tabwidget.h
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
#include <bmds/stringview.h>
#include <bmds/tabrow.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class Menu;

/**
 * @brief A row of tabs over stacked pages, one shown (`devplan/mediaplayer.md`
 *        §6d, `devplan/tabs.md`).
 *
 * Its pages are its children, added with `addTab()`. Only the current page is
 * visible: the others aren't laid out or painted. A click on a tab, or with
 * the focus in a page Ctrl+Tab, Ctrl+PageDown (the next, around the ends),
 * Ctrl+Shift+Tab, Ctrl+PageUp (the previous), changes the page; the focus
 * goes to the new page's first focusable widget if it was in the old one.
 *
 * A tab may have a badge (a few characters in a pill after its title), an
 * attention level (its title in the highlight's color for activity, the close
 * box's for an alert; cleared when the user changes to it) and a close button
 * (drawn on the current tab and the one under the pointer; a middle click
 * too), which asks `CloseRequested`: the owner closes it, or not. A right
 * click on a tab asks `MenuRequested`. A row too narrow scrolls (`TabRow`):
 * three buttons at its right go back, forward, and open a menu of the tabs
 * (the 32 nearest the current one); the wheel over the row scrolls it, and
 * the current tab is always scrolled into view. Without its row
 * (`setBarVisible(false)`) it is a stack of pages that its owner chooses
 * from.
 *
 * Its tabs are kept in arrays that grow 16 at a time (`MemSpace::Internal`),
 * up to `MaxTabs`.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class TabWidget : public Widget {
        public:
                /** @brief Told the page the user changed to (on the loop's thread). */
                using Changed = void (*)(void *arg, TabWidget &tabs, size_t index);

                /** @brief Told a tab's close button was clicked, or the tab middle-clicked. */
                using CloseRequested = void (*)(void *arg, TabWidget &tabs, size_t index);

                /** @brief Told a tab was right-clicked, at (@p x, @p y) in its window's client area. */
                using MenuRequested = void (*)(void *arg, TabWidget &tabs, size_t index, int x, int y);

                static constexpr size_t MaxTabs = 256;
                static constexpr size_t MaxTitle = 31;
                static constexpr size_t MaxBadge = 7;

                /** @brief `attention()`'s levels. */
                static constexpr uint8_t Quiet = 0;
                static constexpr uint8_t Activity = 1;
                static constexpr uint8_t Alert = 2;

                /** @brief Returns the tabs' row (the current one 2 pixels taller): 24, or its font's ink box and 8. */
                uint16_t rowHeight() const {
                        const uint16_t h = uint16_t(textHeight() + 8);
                        return h > 24 ? h : 24;
                }
                static constexpr uint16_t Inset = 4; ///< A page from the frame's outer edge.

                explicit TabWidget(Widget *parent);
                ~TabWidget() override;

                /**
                 * @brief Adds @p page (one of its children) under a tab titled @p title
                 *        (cut at `MaxTitle` bytes on a character) at the end; the first is current.
                 * @return `Ok`, `Full` (`MaxTabs`), `NoMem`, or `Invalid` (not its child).
                 */
                Error addTab(Widget *page, StringView title);

                /** @brief Returns the tabs. */
                size_t count() const { return _count; }

                /** @brief Returns the current page's index. */
                size_t current() const { return _current; }

                /** @brief Returns page @p index, or null. */
                Widget *page(size_t index) const { return index < _count ? _tabs[index].page : nullptr; }

                /** @brief Returns @p page's index, or -1. */
                int indexOf(const Widget *page) const;

                /**
                 * @brief Takes tab @p index away, its page hidden but still its child (the caller
                 *        destroys it or adds it again); the next tab (or the one before, at the
                 *        end) becomes current.
                 */
                void removeTab(size_t index);

                /** @brief Moves tab @p from to @p to (the others shift; past the end: the last). */
                void moveTab(size_t from, size_t to);

                /** @brief Shows page @p index (not told to `Changed`), its tab scrolled into view. */
                void setCurrent(size_t index);

                /** @brief Sets tab @p index's title. */
                void setTitle(size_t index, StringView title);

                /** @brief Sets tab @p index's badge (cut at `MaxBadge` bytes on a character; empty: none). */
                void setBadge(size_t index, StringView badge);

                /** @brief Returns tab @p index's badge. */
                StringView badge(size_t index) const {
                        return index < _count ? StringView(_tabs[index].badge) : StringView();
                }

                /** @brief Sets tab @p index's attention (`Quiet`, `Activity`, `Alert`; more: `Alert`). */
                void setAttention(size_t index, uint8_t level);

                /** @brief Returns tab @p index's attention. */
                uint8_t attention(size_t index) const { return index < _count ? _tabs[index].attention : 0; }

                /** @brief Gives tab @p index a close button, or takes it away. */
                void setClosable(size_t index, bool closable);

                /** @brief Returns true if tab @p index has a close button. */
                bool isClosable(size_t index) const { return index < _count && _tabs[index].closable; }

                /** @brief Shows the row (the default), or not: the pages stacked, flush in it. */
                void setBarVisible(bool visible);

                /** @brief Returns true if the row is shown. */
                bool isBarVisible() const { return _barVisible; }

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                /** @brief Has @p fn told of close buttons clicked. */
                void setCloseRequested(CloseRequested fn, void *arg) {
                        _closeRequested = fn;
                        _closeRequestedArg = arg;
                        return;
                }

                /** @brief Has @p fn told of right clicks on tabs. */
                void setMenuRequested(MenuRequested fn, void *arg) {
                        _menuRequested = fn;
                        _menuRequestedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseLeft() override;
                bool    wheelTurned(int notches) override;
                bool    keyBeforeChild(const KeyEvent &event) override;

        protected:
                void resized() override;

        private:
                struct Tab {
                                Widget       *page = nullptr;
                                char          title[MaxTitle + 1] = {};
                                char          badge[MaxBadge + 1] = {};
                                uint8_t       attention = 0;
                                bool          closable = false;
                                uint16_t      titleWidth = 0; ///< Measured by layout().
                                TabRow::Parts parts;
                };

                Error       grow();
                void        layout(); // the tabs placed, the scroll kept in range, the current page sized
                Rect        pageRect() const;
                Rect        rowRect() const; // the tabs' and buttons' row, from its left
                void        userSet(size_t index);
                void        showCurrent(); // the current tab scrolled into view
                void        scrollTo(uint32_t scroll);
                void        syncFirst(); // _first from _scroll
                TabRow::Hit hitAt(int x, int y) const;
                void        openList();
                void        paintTab(Painter &painter, size_t i, int x, bool on);
                void        paintButtons(Painter &painter, const Rect &row);
                static void listChosen(void *arg, int item);

                Tab              *_tabs = nullptr; // _capacity of each
                uint32_t         *_lefts = nullptr;
                uint16_t         *_widths = nullptr;
                uint16_t         *_closes = nullptr;
                size_t            _capacity = 0;
                size_t            _count = 0;
                size_t            _current = 0;
                uint32_t          _content = 0; // the tabs' width together
                uint32_t          _scroll = 0;  // always _lefts[_first]: the row starts with a whole tab
                size_t            _first = 0;
                TabRow::Bar       _bar;
                int               _hoverTab = -1;
                TabRow::Hit::Kind _hoverKind = TabRow::Hit::Kind::None;
                int               _pressedClose = -1; // a close button pressed: closes if released on it
                bool              _barVisible = true;
                bool              _hoverRow = false;
                Menu             *_list = nullptr; // PSRAM, made at its first opening
                Changed           _changed = nullptr;
                void             *_changedArg = nullptr;
                CloseRequested    _closeRequested = nullptr;
                void             *_closeRequestedArg = nullptr;
                MenuRequested     _menuRequested = nullptr;
                void             *_menuRequestedArg = nullptr;
};

BMDS_NAMESPACE_END
