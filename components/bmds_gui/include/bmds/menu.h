/**
 * @file      menu.h
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
#include <bmds/guiwindow.h>
#include <bmds/menulayout.h>
#include <bmds/mnemonic.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A popup menu (`devplan/gui.md` §4b): items, separators and
 *        submenus, drawn by `drawMenu()` as the window manager's window menu
 *        is.
 *
 * `popup()` opens it on the popup level next to an anchor: a point (a context
 * menu), a button (below it), or a parent item (a submenu, beside it). The
 * pointer highlights an item; a release over one chooses it, so a press on a
 * button, a drag and a release work, as do two clicks. Up, Down, Return and
 * Escape work; Right opens a submenu and Left closes it. Choosing closes the
 * menu and its parents and tells the root menu's `Triggered` callback the
 * item's number. A press outside every popup closes them all (the window
 * manager's rule).
 *
 * @par Thread Safety
 * The application's lock (handlers and drawing hold it).
 */
class Menu : public GuiWindow {
        public:
                /** @brief Told the number of the item chosen (on the loop's thread). */
                using Triggered = void (*)(void *arg, int item);

                static constexpr size_t MaxItems = MenuLayout::MaxItems;
                static constexpr size_t MaxText = 47;
                static constexpr size_t MaxShortcut = 15;

                explicit Menu(GuiApplication &app);
                ~Menu() override;

                /**
                 * @brief Adds an item numbered @p item (`MenuLayout::Flag`s: `Disabled`,
                 *        `Checked`), its shortcut's text shown at the right.
                 * @return `Ok` or `Full`.
                 */
                Error addItem(StringView text, int item, uint8_t flags = 0, StringView shortcut = StringView(),
                              const Icon *icon = nullptr);

                /** @brief Adds a line. @return `Ok` or `Full`. */
                Error addSeparator();

                /** @brief Adds an item opening @p submenu (kept by the caller). @return `Ok` or `Full`. */
                Error addSubmenu(StringView text, Menu &submenu, const Icon *icon = nullptr);

                /** @brief Removes every item (closing it). */
                void clear();

                /** @brief Returns the items. */
                size_t count() const { return _count; }

                /** @brief Told the menu closed (chosen from, dismissed, or closed by a press elsewhere). */
                using Dismissed = void (*)(void *arg, Menu &menu);

                /** @brief Has @p fn told when it (a menu not open as a submenu) closes. */
                void setDismissed(Dismissed fn, void *arg) {
                        _dismissed = fn;
                        _dismissedArg = arg;
                        return;
                }

                /** @brief Told of Left (@p step -1) or Right (1) its tree didn't use (a menu bar's next menu). */
                using Moved = void (*)(void *arg, Menu &menu, int step);

                /**
                 * @brief Has @p fn told of Left in it (opened as no submenu) and Right
                 *        on an item without a submenu, in it or its submenus.
                 */
                void setMoved(Moved fn, void *arg) {
                        _moved = fn;
                        _movedArg = arg;
                        return;
                }

                /** @brief Has @p fn told the item chosen. */
                void setTriggered(Triggered fn, void *arg) {
                        _triggered = fn;
                        _triggeredArg = arg;
                        return;
                }

                /**
                 * @brief Opens it for @p anchor (screen coordinates: a point, a
                 *        button, or with @p submenu a parent item).
                 * @return `Ok`, `Invalid` (no items), or the window manager's error.
                 */
                Error popup(const Rect &anchor, bool submenu = false);

                /** @brief Highlights its first item that can be chosen (opened from the keyboard). */
                void highlightFirst() { highlight(_layout.next(-1, 1)); }

                /** @brief Closes it, its open submenu, and its parents. */
                void dismiss();

                bool keyPressed(const KeyEvent &event) override;
                void paint(Painter &painter) override;
                void mousePressed(const WindowManager::MouseEvent &event) override;
                void mouseReleased(const WindowManager::MouseEvent &event) override;
                void mouseMoved(const WindowManager::MouseEvent &event) override;
                void mouseLeft() override;

        protected:
                void closedByManager() override;

        private:
                struct Item {
                                char        text[MaxText + 1] = {};
                                char        shortcut[MaxShortcut + 1] = {};
                                int         number = 0;
                                uint8_t     flags = 0;
                                Menu       *submenu = nullptr;
                                const Icon *icon = nullptr; ///< Kept by the caller.
                                size_t      underline = Mnemonic::None; ///< Its key's character (`&` taken out).
                                uint32_t    key = 0;
                };

                Error add(StringView text, int number, uint8_t flags, StringView shortcut, Menu *submenu,
                          const Icon *icon);
                void  highlight(int item);
                void  choose(int item);
                void  openSubmenu(int item, bool select);
                void  closeSubmenu();
                Menu &root();
                void  closeTree();
                void  chooseMnemonic(uint32_t key);

                Item             _items[MaxItems];
                MenuLayout::Item _views[MaxItems]; // the items as laid out (set while open)
                size_t           _count = 0;
                MenuLayout       _layout;
                int              _highlight = -1;
                Menu            *_parent = nullptr; // open as its submenu
                Menu            *_open = nullptr;   // its submenu open
                Triggered        _triggered = nullptr;
                void            *_triggeredArg = nullptr;
                Dismissed        _dismissed = nullptr;
                void            *_dismissedArg = nullptr;
                Moved            _moved = nullptr;
                void            *_movedArg = nullptr;
};

BMDS_NAMESPACE_END
