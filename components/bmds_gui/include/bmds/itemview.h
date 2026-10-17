/**
 * @file      itemview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/itemlayout.h>
#include <bmds/itemmodel.h>
#include <bmds/itemselection.h>
#include <bmds/scrollarea.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The base of the item views (`devplan/filebrowser.md` §4g): a model,
 *        the selection and the current row, the mouse and keys. Subclasses
 *        give the geometry and draw.
 *
 * The mouse: a press selects its row (Ctrl toggles it, Shift selects from the
 * anchor); a press on a row already selected keeps the selection until its
 * release (so that the selection can be dragged); a press on no row starts a
 * rubber band (Ctrl adds to the selection). A double click activates. A right
 * press asks for a menu, its row selected first. A slow second click on a
 * selected row's name, or a press held still `LongPressMs`, asks for a rename.
 *
 * The keys: arrows, Home, End, Page Up and Page Down move the current row
 * (Shift extends the selection, Ctrl moves without selecting); Ctrl+Space
 * toggles, Ctrl+A selects all; Enter activates, F2 renames, Escape clears the
 * selection, the Menu key and Shift+F10 ask for a menu; other keys are offered
 * to `KeyTyped`, then a letter goes to the next row starting with it.
 *
 * The owner changes its model, then calls `modelReset()`.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ItemView : public ScrollArea {
        public:
                /** @brief Told row @p row was activated (a double click, Enter). */
                using Activated = void (*)(void *arg, ItemView &view, size_t row);

                /** @brief Told of a menu asked for on row @p row (-1: none) at (@p screenX, @p screenY). */
                using MenuAsked = void (*)(void *arg, ItemView &view, int row, uint16_t screenX, uint16_t screenY);

                /** @brief Told to rename row @p row. */
                using RenameAsked = void (*)(void *arg, ItemView &view, size_t row);

                /** @brief Offered a key the view doesn't use. @return True if used. */
                using KeyTyped = bool (*)(void *arg, ItemView &view, const KeyEvent &event);

                /** @brief Told the selection or the current row changed. */
                using Changed = void (*)(void *arg, ItemView &view);

                /**
                 * @brief Asked to drag the selection (`devplan/clipboard.md` §5d): fill @p drag's
                 *        types, provider, actions and end (the view draws the image).
                 *        @return False: no drag.
                 */
                using DragAsked = bool (*)(void *arg, ItemView &view, GuiDrag &drag);

                /**
                 * @brief Asked what a drop on row @p row (-1: the view's background) would do:
                 *        the action, or `None`. The owner may set @p row to -1 (the drop goes to
                 *        the view itself: a file's row in a folder). A row answered is framed.
                 */
                using DropAsked = DropAction (*)(void *arg, ItemView &view, int &row, const GuiDropEvent &event);

                /** @brief Told of a drop on row @p row (as `DropAsked` set it). @return True if taken (finished later). */
                using Dropped = bool (*)(void *arg, ItemView &view, int row, GuiDropEvent &event);

                /** @brief A drag held over a row this long: `dropHeld()` (a tree's folder opens). */
                static constexpr uint32_t DropHoldMs = 800;

                /** @brief A drag this near the viewport's edge scrolls it. */
                static constexpr int DropScrollZone = 16;

                /** @brief A press held this long, still, renames. */
                static constexpr uint32_t LongPressMs = 800;

                /** @brief A press moving more than this many pixels drags. */
                static constexpr int DragDistance = 4;

                explicit ItemView(Widget *parent);
                ~ItemView() override;

                /** @brief Sets the model (kept by the caller; null: none), and resets. */
                void setModel(ItemModel *model);

                /** @brief Returns the model, or null. */
                ItemModel *model() const { return _model; }

                /** @brief Rows count again from the model: the selection cleared, laid out, redrawn. */
                void modelReset();

                /**
                 * @brief `modelReset()`, then @p rows selected and @p current current (-1:
                 *        none), told once: a sort or a refresh keeping the selection (the
                 *        owner finds the rows again by name). Rows past the count are passed over.
                 */
                void resetTo(Span<const size_t> rows, int current);

                /** @brief Returns the rows. */
                size_t count() const { return _selection.count(); }

                /** @brief Returns the selection. */
                const ItemSelection &selection() const { return _selection; }

                /** @brief Returns the current row, or -1. */
                int current() const { return _selection.current(); }

                /** @brief Selects only @p row (-1: none), current and shown. */
                void selectOnly(int row);

                /** @brief Selects or deselects @p row alone. */
                void setSelected(size_t row, bool selected);

                /** @brief Makes @p row current without changing the selection. */
                void setCurrent(int row);

                /** @brief Allows one row selected at most (a tree). */
                void setSingleSelection(bool single) { _single = single; }

                /** @brief Scrolls as little as shows @p row. */
                void showRow(size_t row);

                /** @brief Hides @p row's name while an editor is over it (-1: none). */
                void setEditing(int row);

                /**
                 * @brief Returns the `KeyEvent::Modifier` bits held with the activation being
                 *        told (Shift and a double click, Shift+Enter), else 0.
                 */
                uint8_t activationModifiers() const { return _activation; }

                /** @brief Returns the row whose name an editor is over, or -1. */
                int editing() const { return _editing; }

                /**
                 * @brief Returns where row @p row's name is edited (screen coordinates):
                 *        its first line, as wide as an editor may be.
                 */
                virtual Rect nameRect(size_t row) const = 0;

                void setActivated(Activated fn, void *arg) {
                        _activated = fn;
                        _activatedArg = arg;
                        return;
                }

                void setMenuAsked(MenuAsked fn, void *arg) {
                        _menuAsked = fn;
                        _menuArg = arg;
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

                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                void setDragAsked(DragAsked fn, void *arg) {
                        _dragAsked = fn;
                        _dragArg = arg;
                        return;
                }

                /** @brief Makes it a drop target (null @p askedFn: not one). */
                void setDropHandlers(DropAsked askedFn, Dropped droppedFn, void *arg) {
                        _dropAsked = askedFn;
                        _dropped = droppedFn;
                        _dropArg = arg;
                        setAcceptsDrops(askedFn != nullptr);
                        return;
                }

                /** @brief Returns where the press that started this view's last drag was (window coordinates). */
                void dragOrigin(int &x, int &y) const {
                        x = _pressX;
                        y = _pressY;
                        return;
                }

                /** @brief Returns the row a drag over the view is framed on, or -1. */
                int dropRow() const { return _dropRow; }

                void dragEntered(GuiDropEvent &event) override;
                void dragMoved(GuiDropEvent &event) override;
                void dragLeft() override;
                bool dropped(GuiDropEvent &event) override;

                void mousePressed(const WindowManager::MouseEvent &event) override;
                void mouseMoved(const WindowManager::MouseEvent &event) override;
                void mouseReleased(const WindowManager::MouseEvent &event) override;
                void mouseLeft() override;
                bool keyPressed(const KeyEvent &event) override;

        protected:
                /** @brief Returns the row at content point (@p x, @p y), or -1. */
                virtual int rowAt(uint32_t x, uint32_t y) const = 0;

                /** @brief Returns true if content point (@p x, @p y) is on row @p row's name. */
                virtual bool onName(size_t row, uint32_t x, uint32_t y) const = 0;

                /** @brief Returns row @p row's rectangle, in content coordinates. */
                virtual ContentRect rowRect(size_t row) const = 0;

                /** @brief Returns the row an arrow (@p dx, @p dy) goes to from @p row. */
                virtual size_t neighbour(size_t row, int dx, int dy) const = 0;

                /** @brief Returns the row @p pages pages from @p row (Page Up and Down). */
                virtual size_t pageNeighbour(size_t row, int pages) const;

                /** @brief Returns the first (or @p last) row as Home and End go. */
                virtual size_t endRow(bool last) const { return last && count() > 0 ? count() - 1 : 0; }

                /**
                 * @brief Returns true if row @p row is in a Shift range from @p anchor to @p to:
                 *        between them in the rows' order by default (a grid: its rectangle).
                 */
                virtual bool inRange(size_t row, size_t anchor, size_t to) const {
                        return anchor <= to ? row >= anchor && row <= to : row >= to && row <= anchor;
                }

                /** @brief Returns the rows in the order a typed letter looks through them; row @p k. */
                virtual size_t typingOrder(size_t k) const { return k; }

                /** @brief Lays out again: the content's size from the rows (the model changed, or the size). */
                virtual void relayout() = 0;

                /**
                 * @brief Draws row @p row into a drag's image (window coordinates): its small
                 *        icon and its name on the highlight, by default.
                 */
                virtual void drawDragRow(DragPicture &picture, size_t row, const WindowPalette &palette) const;

                /** @brief A drag was held over row @p row `DropHoldMs` (a tree opens a folder). */
                virtual void dropHeld(size_t row) { (void)row; }

                /** @brief Returns row @p row's rectangle in window coordinates (not cut to the viewport). */
                DragImage::Box rowBox(size_t row) const;

                /** @brief Row @p row was activated (a double click, Enter): told to `Activated`. */
                virtual void activate(size_t row);

                /** @brief Draws the rows. */
                virtual void paintRows(Painter &painter, const Rect &viewport) = 0;

                /**
                 * @brief `modelReset()`, then row @p row selected and current, told to
                 *        `Changed` only if @p tell (a tree's rows moved, the same one kept).
                 */
                void resetSelecting(int row, bool tell);

                /**
                 * @brief Draws a row's label background (window coordinates, an 18-pixel row from
                 *        @p y, the label @p x to @p x + @p width): the highlight while selected
                 *        and focused, the face while selected only (an inactive selection).
                 * @return The colour the text goes on.
                 */
                static uint32_t drawLabel(Painter &painter, const Rect &clip, int x, int y, int width, bool selected,
                                          bool focus, bool current, uint32_t background);

                /** @brief Draws the focus rectangle around a label, after its text, outside its rows. */
                static void focusRect(Painter &painter, const Rect &clip, int x, int y, int width);

                /** @brief Returns true if the view shows its focus: it has the keys and its window is active. */
                bool showsFocus() const;

                /** @brief Asks for row @p row to be redrawn. */
                void updateRow(size_t row);

                /** @brief Returns the window rectangle of content rectangle @p r, cut to the viewport. */
                Rect toWindow(const ContentRect &r) const;

                /** @brief Returns content x of window x @p x. */
                uint32_t contentX(int x) const { return uint32_t(int(scrollX()) + x - int(viewport().x)); }

                /** @brief Returns content y of window y @p y. */
                uint32_t contentY(int y) const { return uint32_t(int(scrollY()) + y - int(viewport().y)); }

                void paintContent(Painter &painter, const Rect &viewport) override;
                void resized() override;

        private:
                void        changed(); // the selection or current row: told, redrawn
                void        moveTo(size_t row, bool shift, bool ctrl);
                void        selectRangeTo(size_t row, bool add);
                void        menuAt(int row, int x, int y); // window coordinates
                void        band(int x, int y);            // the rubber band to window point (x, y)
                void        showBand(bool shown);          // the window manager's band at _band, or none
                void        stopBandScroll();
                static void bandDue(void *arg); // the pointer past the viewport's edge: scrolled
                int         startingWith(uint32_t key) const;
                void        startRename(int row, uint32_t ms);
                void        stopRename();
                static void renameDue(void *arg);
                void        beginDrag();                   // the press moved past DragDistance
                int         dropRowAt(int x, int y) const; // -1: the background (or outside the rows)
                void        setDropRow(int row);
                void        dropScroll(int x, int y); // near the edge: scrolled while it stays
                void        stopDropTimers();
                static void dropScrollDue(void *arg);
                static void dropHoldDue(void *arg);

                ItemModel    *_model = nullptr;
                ItemSelection _selection;
                Activated     _activated = nullptr;
                void         *_activatedArg = nullptr;
                MenuAsked     _menuAsked = nullptr;
                void         *_menuArg = nullptr;
                RenameAsked   _renameAsked = nullptr;
                void         *_renameArg = nullptr;
                KeyTyped      _keyTyped = nullptr;
                void         *_keyArg = nullptr;
                Changed       _changed = nullptr;
                void         *_changedArg = nullptr;
                uint64_t      _pressUs = 0;   // the last left press
                int           _pressRow = -1; // its row (a double click is on the same one)
                uint8_t        _activation = 0; // the modifiers of the activation being told
                int           _pressed = -1;  // the row a left press is on, until its release
                int           _pressX = 0;    // where (window coordinates)
                int           _pressY = 0;
                bool          _dragging = false;
                bool          _onlyOnRelease = false; // a press on a selected row: select only it if not dragged
                bool          _onSelectedName = false;
                bool          _banding = false;
                bool          _bandAdds = false; // Ctrl at its press: it only adds
                uint32_t      _bandX = 0;        // its first corner (content coordinates)
                uint32_t      _bandY = 0;
                ContentRect   _band;
                int            _bandPointerX = 0; // where the pointer is (window coordinates)
                int            _bandPointerY = 0;
                int            _bandTimer = -1; // scrolling while the pointer is past the edge
                int           _renameTimer = -1;
                int           _renameRow = -1;
                bool           _rightPressed = false; // a right press: its menu at the release, unless dragged
                int            _rightRow = -1;
                DragAsked      _dragAsked = nullptr;
                void          *_dragArg = nullptr;
                DropAsked      _dropAsked = nullptr;
                Dropped        _dropped = nullptr;
                void          *_dropArg = nullptr;
                int            _dropRow = -1; // framed
                int            _dropX = 0;    // a drag's place over the view (window coordinates)
                int            _dropY = 0;
                int            _dropScrollTimer = -1;
                int            _dropHoldTimer = -1;
                int            _dropHoldRow = -1;
                int           _editing = -1;
                bool          _single = false;
                List<uint32_t> _drawn{MemSpace::Psram}; // the selection as last drawn (its words)
                int            _drawnCurrent = -1;      // the current row as last drawn
};

BMDS_NAMESPACE_END
