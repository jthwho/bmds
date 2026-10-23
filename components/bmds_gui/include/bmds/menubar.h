/**
 * @file      menubar.h
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
#include <bmds/menu.h>
#include <bmds/mnemonic.h>
#include <bmds/stringview.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class MenuTitle;

/**
 * @brief A window's menu bar (`devplan/filebrowser.md` §4e): titles opening
 *        `Menu`s below them.
 *
 * A press on a title opens its menu (a press while it is open closes it: the
 * window manager's rule for popups). With a menu open, the pointer moving
 * onto another title opens that one; Left and Right go round the bar. Alt and
 * a title's marked letter (`&File`) opens it from the keyboard, its first item
 * highlighted, as does F10 for the first (`GuiWindow::setMenuBar()`). The
 * open title is drawn sunken.
 *
 * The bar sets its menus' `Dismissed` and `Moved` callbacks as it opens
 * them, so bars may share menus (one window's open at a time); `Triggered`
 * is the caller's. `setAboutToShow()`'s function fills a menu for its bar
 * just before it opens.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class MenuBar : public Widget {
        public:
                static constexpr size_t   MaxMenus = 10;
                static constexpr size_t   MaxText = 23;
                static constexpr uint16_t Height = 20; ///< A font row and 2 pixels above and below.

                /** @brief Told that @p menu (title @p index's) is about to open from @p bar. */
                using AboutToShow = void (*)(void *arg, MenuBar &bar, size_t index, Menu &menu);

                explicit MenuBar(Widget *parent);

                /** @brief Has @p fn told before each menu opens (null: none). */
                void setAboutToShow(AboutToShow fn, void *arg) {
                        _aboutToShow = fn;
                        _aboutToShowArg = arg;
                        return;
                }

                /**
                 * @brief Adds a title (`&` marking its key) opening @p menu (kept by
                 *        the caller).
                 * @return `Ok`, `Full`, or `NoMem`.
                 */
                Error addMenu(StringView title, Menu &menu);

                /** @brief Sets title @p index's text (`&` marking its key). */
                void setTitle(size_t index, StringView title);

                /** @brief Takes every menu away (closing an open one); the menus stay their owner's. */
                void clear();

                /** @brief Returns the titles. */
                size_t count() const { return _count; }

                /**
                 * @brief Opens title @p index's menu (@p keyboard: its first item
                 *        highlighted).
                 * @return True if it opened.
                 */
                bool open(size_t index, bool keyboard);

                /** @brief Opens the first menu from the keyboard (F10). @return True if it opened. */
                bool openFirst() { return _count > 0 && open(0, true); }

                /** @brief Returns the open title, or -1. */
                int openIndex() const { return _open; }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        protected:
                void resized() override;

        private:
                friend class MenuTitle;

                static void dismissed(void *arg, Menu &menu);
                static void moved(void *arg, Menu &menu, int step);
                int         indexOf(const Menu &menu) const;
                void        hovered(size_t index); // the pointer onto a title

                MenuTitle *_titles[MaxMenus] = {};
                Menu      *_menus[MaxMenus] = {};
                size_t     _count = 0;
                int        _open = -1;
                bool       _opening = false; // a menu is being swapped for another: its dismissal not ours
                AboutToShow _aboutToShow = nullptr;
                void       *_aboutToShowArg = nullptr;
};

BMDS_NAMESPACE_END
