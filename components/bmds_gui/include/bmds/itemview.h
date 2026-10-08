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

                /** @brief Returns the rows in the order a typed letter looks through them; row @p k. */
                virtual size_t typingOrder(size_t k) const { return k; }

                /** @brief Lays out again: the content's size from the rows (the model changed, or the size). */
                virtual void relayout() = 0;

                /**
                 * @brief A press on a row moved past `DragDistance` by (@p dx, @p dy)
                 *        (window pixels from the press), its button held; @p done on
                 *        the release. @return True if the subclass moves it (the
                 *        desktop's grid).
                 */
                virtual bool dragged(int dx, int dy, bool done) {
                        (void)dx;
                        (void)dy;
                        (void)done;
                        return false;
                }

                /** @brief The press's drag was cut off (a popup opened). */
                virtual void dragCancelled() {}

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
                void        menuAt(int row, int x, int y); // window coordinates
                void        band(int x, int y);            // the rubber band to window point (x, y)
                void        showBand(bool shown);          // the window manager's band at _band, or none
                int         startingWith(uint32_t key) const;
                void        startRename(int row, uint32_t ms);
                void        stopRename();
                static void renameDue(void *arg);

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
                int           _renameTimer = -1;
                int           _renameRow = -1;
                int           _editing = -1;
                bool          _single = false;
                List<uint32_t> _drawn{MemSpace::Psram}; // the selection as last drawn (its words)
                int            _drawnCurrent = -1;      // the current row as last drawn
};

BMDS_NAMESPACE_END
