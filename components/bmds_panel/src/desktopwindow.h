/**
 * @file      desktopwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/desktopentry.h>
#include <bmds/guiwindow.h>
#include <bmds/icongrid.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The desktop behind every window (`devplan/gui.md` §4c, §4m): its
 *        color, and icons with their names, down then across from the work
 *        area's corner, in a grid's cells. A view: the panel gives it each
 *        item's name, icon and wanted cell, and is told when one is opened (a
 *        double click), moved (dragged to another cell), or the menu asked for
 *        (a right click).
 *
 * @par Thread Safety
 * The application's lock, as any widget.
 */
class DesktopWindow final : public GuiWindow {
        public:
                /** @brief Icons at most. */
                static constexpr size_t MaxItems = 32;

                /** @brief An icon's side, in pixels. */
                static constexpr uint16_t IconSide = 32;

                /** @brief An item's cell: its icon, its name in up to two lines, room around. */
                static constexpr uint16_t CellWidth = 80;
                static constexpr uint16_t CellHeight = 76;

                /** @brief A name's line kept, in bytes (9 characters of up to 4, NUL-ended). */
                static constexpr size_t LineBytes = 40;

                /** @brief Told that item @p item was opened. */
                using Opened = void (*)(void *arg, size_t item);

                /** @brief Told that the items' cells changed (`itemCell()`): one was dragged. */
                using Moved = void (*)(void *arg);

                /** @brief A press moving more than this many pixels drags. */
                static constexpr int DragDistance = 4;

                /** @brief Told of a right press at (@p screenX, @p screenY). */
                using MenuAsked = void (*)(void *arg, uint16_t screenX, uint16_t screenY);

                /** @brief Told of a right press on item @p item (selected now), at (@p screenX, @p screenY). */
                using ItemMenuAsked = void (*)(void *arg, size_t item, uint16_t screenX, uint16_t screenY);

                /**
                 * @brief Told to rename item @p item (`devplan/gui.md` §4q): a click on a
                 *        selected item's name once the double-click time has passed, or a
                 *        press held still `LongPressMs`.
                 */
                using RenameAsked = void (*)(void *arg, size_t item);

                /** @brief A press held this long, still, renames. */
                static constexpr uint32_t LongPressMs = 800;

                /**
                 * @brief Offered a key the desktop doesn't use itself (Delete, Ctrl+C...),
                 *        with the selected item (-1: none). @return True if used.
                 */
                using KeyTyped = bool (*)(void *arg, const KeyEvent &event, int selected);

                DesktopWindow(GuiApplication &app, const Config &config) : GuiWindow(app, config) {}
                ~DesktopWindow() override;

                void setOpened(Opened fn, void *arg) {
                        _opened = fn;
                        _openedArg = arg;
                        return;
                }

                void setMoved(Moved fn, void *arg) {
                        _moved = fn;
                        _movedArg = arg;
                        return;
                }

                void setMenuAsked(MenuAsked fn, void *arg) {
                        _menuAsked = fn;
                        _menuArg = arg;
                        return;
                }

                void setItemMenuAsked(ItemMenuAsked fn, void *arg) {
                        _itemMenuAsked = fn;
                        _itemMenuArg = arg;
                        return;
                }

                void setRenameAsked(RenameAsked fn, void *arg) {
                        _renameAsked = fn;
                        _renameArg = arg;
                        return;
                }

                void setKeyTyped(KeyTyped fn, void *arg) {
                        _keyTyped = fn;
                        _keyArg = arg;
                        return;
                }

                /** @brief Returns the selected item, or -1. */
                int selected() const { return _selected; }

                /**
                 * @brief Puts item @p item in the cell at (@p screenX, @p screenY) if that
                 *        cell is on the grid and free (a new file where the menu was opened).
                 * @return True if it moved.
                 */
                bool placeItem(size_t item, uint16_t screenX, uint16_t screenY);

                /** @brief Selects item @p item (-1: none). */
                void setSelected(int item) { select(item >= 0 && size_t(item) < _count ? item : -1); }

                /**
                 * @brief Returns where item @p item's name is edited: its cell's left and
                 *        width, its name's first line's top, a line's height (screen
                 *        coordinates; `PopupEdit::Look::IconName` grows it down).
                 */
                Rect nameRect(size_t item) const;

                /** @brief Hides item @p item's name while an editor is over it (-1: none). */
                void setEditing(int item);

                /**
                 * @brief Shows @p names as items (`MaxItems` at most) in the cells
                 *        of a grid filling @p area (window coordinates), each in the
                 *        cell of @p wanted it wants if it can (`IconGrid::place()`;
                 *        @p wanted as many as @p names, or empty), without icons yet;
                 *        the earlier ones go.
                 * @return `Ok` or `NoMem`.
                 */
                Error setItems(Span<const StringView> names, Span<const IconGrid::Cell> wanted, const Rect &area);

                /** @brief Returns item @p item's cell. */
                IconGrid::Cell itemCell(size_t item) const {
                        return item < _count ? _items[item].at : IconGrid::Cell();
                }

                /** @brief Returns the items shown. */
                size_t itemCount() const { return _count; }

                /** @brief Sets item @p item's icon to a copy of @p icon (null: none). */
                void setItemIcon(size_t item, const Icon *icon);

                /** @brief Returns true if item @p item has an icon. */
                bool hasItemIcon(size_t item) const { return item < _count && _items[item].pixels != nullptr; }

                void paint(Painter &painter) override;
                void mousePressed(const WindowManager::MouseEvent &event) override;
                void mouseMoved(const WindowManager::MouseEvent &event) override;
                void mouseReleased(const WindowManager::MouseEvent &event) override;
                void mouseLeft() override;
                bool keyPressed(const KeyEvent &event) override;

        protected:
                void activeChanged() override;
                void drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                   const WindowPalette &palette) override;

        private:
                // An item: its name's lines, and where its parts are.
                struct Item {
                                char           text[2][LineBytes];
                                uint8_t        lines;
                                uint16_t       textX[2]; // from the cell's left, on 4-pixel columns
                                Rect           cell;     // where it is drawn: its grid cell's, or dragged
                                IconGrid::Cell at;
                                uint32_t      *pixels; // the icon's copy, or null
                                uint16_t       iconWidth;
                                uint16_t       iconHeight;
                };

                void freeItems();
                Rect cellRect(IconGrid::Cell at) const;
                void moveTo(Item &item, const Rect &cell);
                int  itemAt(int x, int y) const;
                void select(int item);
                Rect iconRect(const Item &item) const;
                Rect lineRect(const Item &item, int line) const; // its background, selected
                Rect        hitRect(const Item &item) const;
                int         nearest(int dx, int dy) const; // the item nearest the selection that way
                int         byGrid(bool last) const;       // the first (or last) item down then across
                int         startingWith(uint32_t key) const;
                Rect        menuPoint() const;                  // where the keyboard opens a menu (screen coordinates)
                void        startRename(int item, uint32_t ms); // the timer
                void        stopRename();
                static void renameDue(void *arg);
                // The item drawn @p k-th: in order, the one dragged last (above the others).
                size_t drawnAt(size_t k) const {
                        if (!_dragging) return k;
                        const size_t p = size_t(_pressed);
                        return k + 1 == _count ? p : (k < p ? k : k + 1);
                }
                Item     *_items = nullptr;
                size_t    _count = 0;
                int       _selected = -1;
                uint64_t  _pressUs = 0;  // the last press on the selected item
                int       _pressed = -1; // the item a left press is on, until its release
                int       _pressX = 0;   // where, from the item's cell
                int       _pressY = 0;
                bool      _dragging = false;
                uint16_t  _left = 0; // the grid: its top-left cell's corner, its size in cells
                uint16_t  _top = 0;
                uint16_t  _columns = 1;
                uint16_t  _rows = 1;
                Moved     _moved = nullptr;
                void     *_movedArg = nullptr;
                Opened    _opened = nullptr;
                void     *_openedArg = nullptr;
                MenuAsked _menuAsked = nullptr;
                void     *_menuArg = nullptr;
                ItemMenuAsked _itemMenuAsked = nullptr;
                void         *_itemMenuArg = nullptr;
                RenameAsked   _renameAsked = nullptr;
                void         *_renameArg = nullptr;
                int           _renameTimer = -1; // a long press, or a slow second click, under way
                int           _renameItem = -1;
                bool          _onSelectedName = false; // the press is on the selected item's name
                int           _editing = -1;           // its name hidden: an editor is over it
                KeyTyped      _keyTyped = nullptr;
                void         *_keyArg = nullptr;
};

BMDS_NAMESPACE_END
