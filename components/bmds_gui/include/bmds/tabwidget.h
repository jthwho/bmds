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
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A row of tabs over stacked pages, one shown (`devplan/mediaplayer.md`
 *        §6d).
 *
 * Its pages are its children, added with `addTab()`. Only the current page is
 * visible: the others aren't laid out or painted. A click on a tab, or with
 * the focus in a page Ctrl+Tab, Ctrl+PageDown (the next, around the ends),
 * Ctrl+Shift+Tab, Ctrl+PageUp (the previous), changes the page; the focus
 * goes to the new page's first focusable widget if it was in the old one.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class TabWidget : public Widget {
        public:
                /** @brief Told the page the user changed to (on the loop's thread). */
                using Changed = void (*)(void *arg, TabWidget &tabs, size_t index);

                static constexpr size_t   MaxTabs = 16;
                static constexpr size_t   MaxTitle = 31;
                static constexpr uint16_t RowHeight = 24; ///< The tabs' row, the current one 2 pixels taller.
                static constexpr uint16_t Inset = 4;      ///< A page from the frame's outer edge.

                explicit TabWidget(Widget *parent);

                /**
                 * @brief Adds @p page (one of its children) under a tab titled @p title
                 *        (cut at `MaxTitle` bytes on a character); the first is current.
                 * @return `Ok`, `Full`, or `Invalid` (not its child).
                 */
                Error addTab(Widget *page, StringView title);

                /** @brief Returns the tabs. */
                size_t count() const { return _count; }

                /** @brief Returns the current page's index. */
                size_t current() const { return _current; }

                /** @brief Returns page @p index, or null. */
                Widget *page(size_t index) const { return index < _count ? _tabs[index].page : nullptr; }

                /**
                 * @brief Takes tab @p index away, its page hidden but still its child (the caller
                 *        destroys it or adds it again); the next tab (or the one before, at the
                 *        end) becomes current.
                 */
                void removeTab(size_t index);

                /** @brief Shows page @p index (not told to `Changed`). */
                void setCurrent(size_t index);

                /** @brief Sets tab @p index's title. */
                void setTitle(size_t index, StringView title);

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                bool    keyBeforeChild(const KeyEvent &event) override;

        protected:
                void resized() override;

        private:
                struct Tab {
                                Widget  *page = nullptr;
                                char     title[MaxTitle + 1] = {};
                                uint16_t left = 0; // placed by layout()
                                uint16_t width = 0;
                };

                void layout(); // the tabs placed, the current page sized
                Rect pageRect() const;
                void userSet(size_t index);

                Tab     _tabs[MaxTabs];
                size_t  _count = 0;
                size_t  _current = 0;
                Changed _changed = nullptr;
                void   *_changedArg = nullptr;
};

BMDS_NAMESPACE_END
