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
#include <bmds/guiwindow.h>
#include <bmds/icongrid.h>
#include <bmds/iconview.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/tiledpattern.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The desktop behind every window (`devplan/gui.md` §4c, §4m): its
 *        color, and an icon view on its grid in the work area
 *        (`devplan/filebrowser.md` §4g). A view: the panel gives it each item's
 *        name, icon and wanted cell, and is told when items are opened (a
 *        double click), moved (dragged to other cells), renamed, or the menu
 *        asked for (a right click), and asked for drags and drops
 *        (`devplan/clipboard.md` §5e): its own icons dropped on its grid move there.
 *        Behind them: the color, or a tiled pattern (`devplan/wallpaper.md`).
 *
 * The icons are selected as any item view's: Ctrl, Shift, a rubber band.
 *
 * @par Thread Safety
 * The application's lock, as any widget.
 */
class DesktopWindow final : public GuiWindow {
        public:
                /** @brief An icon's side, in pixels. */
                static constexpr uint16_t IconSide = IconView::IconSide;

                /** @brief Told that item @p item was opened. */
                using Opened = void (*)(void *arg, size_t item);

                /** @brief Told that the items' cells changed (`itemCell()`): some were dragged. */
                using Moved = void (*)(void *arg);

                /** @brief Told of a right press at (@p screenX, @p screenY). */
                using MenuAsked = void (*)(void *arg, uint16_t screenX, uint16_t screenY);

                /** @brief Told of a right press on item @p item (selected now), at (@p screenX, @p screenY). */
                using ItemMenuAsked = void (*)(void *arg, size_t item, uint16_t screenX, uint16_t screenY);

                /** @brief Told to rename item @p item (`devplan/gui.md` §4q). */
                using RenameAsked = void (*)(void *arg, size_t item);

                /**
                 * @brief Offered a key the desktop doesn't use itself (Delete, Ctrl+C...),
                 *        with the current item if it is selected (-1: none). @return True if used.
                 */
                using KeyTyped = bool (*)(void *arg, const KeyEvent &event, int selected);

                /** @brief Asked to drag the selected items: fill @p drag (the image is the view's). */
                using DragAsked = bool (*)(void *arg, GuiDrag &drag);

                /**
                 * @brief Asked what a drop on item @p item (-1: the desktop itself) does; the
                 *        panel may set @p item to -1 (the drop goes to the desktop's folder).
                 */
                using DropAsked = DropAction (*)(void *arg, int &item, const GuiDropEvent &event);

                /** @brief Told of a drop on item @p item (as `DropAsked` set it). @return True if taken. */
                using Dropped = bool (*)(void *arg, int item, GuiDropEvent &event);

                DesktopWindow(GuiApplication &app, const Config &config);
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

                void setDragAsked(DragAsked fn, void *arg) {
                        _dragAsked = fn;
                        _dragArg = arg;
                        return;
                }

                void setDropHandlers(DropAsked askedFn, Dropped droppedFn, void *arg) {
                        _dropAsked = askedFn;
                        _dropped = droppedFn;
                        _dropArg = arg;
                        return;
                }

                /** @brief Returns the current item if it is selected, else the first selected, or -1. */
                int selected() const;

                /** @brief Returns the items selected. */
                size_t selectedCount() const { return _view != nullptr ? _view->selection().selectedCount() : 0; }

                /** @brief Returns the first selected item at or after @p from, or -1. */
                int nextSelected(size_t from) const {
                        return _view != nullptr ? _view->selection().nextSelected(from) : -1;
                }

                /**
                 * @brief Puts item @p item in the cell at (@p screenX, @p screenY) if that
                 *        cell is on the grid and free (a new file where the menu was opened).
                 * @return True if it moved.
                 */
                bool placeItem(size_t item, uint16_t screenX, uint16_t screenY) {
                        return _view != nullptr && _view->placeRow(item, screenX, screenY);
                }

                /** @brief Selects only item @p item (-1: none). */
                void setSelected(int item);

                /** @brief Returns where item @p item's name is edited (screen coordinates). */
                Rect nameRect(size_t item) const { return _view != nullptr ? _view->nameRect(item) : Rect(); }

                /** @brief Hides item @p item's name while an editor is over it (-1: none). */
                void setEditing(int item) {
                        if (_view != nullptr) _view->setEditing(item);
                        return;
                }

                /**
                 * @brief Shows @p names as items in the cells of a grid filling @p area
                 *        (window coordinates), each in the cell of @p wanted it wants if
                 *        it can (@p wanted as many as @p names, or empty), without icons
                 *        yet; the earlier ones go.
                 * @return `Ok` or `NoMem`.
                 */
                Error setItems(Span<const StringView> names, Span<const IconGrid::Cell> wanted, const Rect &area);

                /** @brief Returns item @p item's cell. */
                IconGrid::Cell itemCell(size_t item) const {
                        return _view != nullptr ? _view->cellOf(item) : IconGrid::Cell();
                }

                /** @brief Returns the items shown. */
                size_t itemCount() const { return _model.count(); }

                /**
                 * @brief Sets item @p item's icon to a copy of @p icon (null: none),
                 *        @p dimmed (a cut file's: its alpha halved, `devplan/filebrowser.md` §4j).
                 */
                void setItemIcon(size_t item, const Icon *icon, bool dimmed);

                /** @brief Returns true if item @p item has an icon. */
                bool hasItemIcon(size_t item) const { return item < _icons.size() && _icons[item].pixels != nullptr; }

                /** @brief Returns true if item @p item's icon is dimmed. */
                bool isItemDimmed(size_t item) const { return item < _icons.size() && _icons[item].dimmed; }

                /**
                 * @brief Shows @p file (a PNG) tiled as the wallpaper, as `TiledPattern::load()` takes
                 *        it; on an error, the color alone.
                 * @return `TiledPattern::load()`'s.
                 */
                Error setPattern(ByteSpan file, StringView colors, unsigned scale);

                /** @brief Shows the desktop's color alone. */
                void clearPattern();

                /** @brief Returns the pattern (invalid: none). */
                const TiledPattern &pattern() const { return _pattern; }

                void paint(Painter &painter) override;

        protected:
                void activeChanged() override;
                void drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                   const WindowPalette &palette) override;

        private:
                // An icon's copy (the theme's cache may drop its own).
                struct OwnedIcon {
                                Icon      icon;
                                uint32_t *pixels = nullptr;
                                bool      dimmed = false;
                };

                void        freeIcons();
                static void activated(void *arg, ItemView &view, size_t row);
                static void menuAsked(void *arg, ItemView &view, int row, uint16_t x, uint16_t y);
                static void renameAsked(void *arg, ItemView &view, size_t row);
                static bool keyTyped(void *arg, ItemView &view, const KeyEvent &event);
                static void moved(void *arg, IconView &view);
                static bool       viewDragAsked(void *arg, ItemView &view, GuiDrag &drag);
                static DropAction viewDropAsked(void *arg, ItemView &view, int &row, const GuiDropEvent &event);
                static bool       viewDropped(void *arg, ItemView &view, int row, GuiDropEvent &event);
                bool              ownOnGrid(int row, const GuiDropEvent &event) const; // its icons moved, not sent

                ListItemModel   _model;
                IconView       *_view = nullptr; // its child, over the work area
                List<OwnedIcon> _icons{MemSpace::Psram};
                TiledPattern    _pattern;
                uint32_t        _desktopColor = 0; // the palette's when the pattern was blended over it
                Opened          _opened = nullptr;
                void           *_openedArg = nullptr;
                Moved           _moved = nullptr;
                void           *_movedArg = nullptr;
                MenuAsked       _menuAsked = nullptr;
                void           *_menuArg = nullptr;
                ItemMenuAsked   _itemMenuAsked = nullptr;
                void           *_itemMenuArg = nullptr;
                RenameAsked     _renameAsked = nullptr;
                void           *_renameArg = nullptr;
                KeyTyped        _keyTyped = nullptr;
                void           *_keyArg = nullptr;
                DragAsked       _dragAsked = nullptr;
                void           *_dragArg = nullptr;
                DropAsked       _dropAsked = nullptr;
                Dropped         _dropped = nullptr;
                void           *_dropArg = nullptr;
};

BMDS_NAMESPACE_END
