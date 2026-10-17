/**
 * @file      listeditor.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/itemmodel.h>
#include <bmds/string.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class DetailsView;
class ItemView;
class PopupEdit;

/**
 * @brief An ordered list of texts to edit (`devplan/gui.md` §4u): a `paths`
 *        setting, the panel's items, the hosts file's names.
 *
 * The items in a one-column list, and buttons beside it: Add, Edit, Remove,
 * Up, Down. Add appends an item edited in place, unless the owner adds its
 * own way (`setAddAsked()`: a folder dialog, then `addItem()`); Edit (F2, a
 * double click, Enter) edits the current one in place, and an item edited to
 * nothing goes. Insert adds, Delete removes, Alt+Up and Alt+Down move.
 *
 * The list is kept as text, its items between a separator (`ListText`):
 * `setText()` and `text()`. `Changed` is told after each change the user
 * makes; `setText()` isn't.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ListEditor : public Widget {
        public:
                /** @brief Told the user changed the list (on the loop's thread). */
                using Changed = void (*)(void *arg, ListEditor &editor);

                /** @brief Told Add was asked for (the owner then calls `addItem()`, or not). */
                using AddAsked = void (*)(void *arg, ListEditor &editor);

                /** @brief The list's rows shown at least. */
                static constexpr size_t ShownRows = 5;

                /** @brief An item's longest text. */
                static constexpr size_t MaxItem = 255;

                ListEditor(Widget *parent, char separator);
                ~ListEditor() override;

                /** @brief Sets the items from @p text (between separators); not told. @return `Ok` or `NoMem`. */
                Error setText(StringView text);

                /** @brief Returns the items, between separators. */
                StringView text() const { return _text.view(); }

                /** @brief Returns the items. */
                size_t count() const;

                /** @brief Returns item @p index (empty past the end). */
                StringView item(size_t index) const;

                /** @brief Returns the list (a label's buddy), or null (no memory). */
                DetailsView *view() const { return _view; }

                /** @brief Sets the list's column title (none by default: no header). */
                void setTitle(StringView title);

                /** @brief Appends @p item (several, if it holds the separator), current, told. @return `Ok` or `NoMem`. */
                Error addItem(StringView item);

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                /** @brief Has @p fn told of Add instead of editing a new item in place (null: in place). */
                void setAddAsked(AddAsked fn, void *arg) {
                        _addAsked = fn;
                        _addAskedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        protected:
                void resized() override;

        private:
                enum Action : uintptr_t {
                        Add,
                        Edit,
                        Remove,
                        Up,
                        Down,
                        ActionCount,
                };

                static void clicked(void *arg, Button &button);
                static void activated(void *arg, ItemView &view, size_t row);
                static void renameAsked(void *arg, ItemView &view, size_t row);
                static bool keyTyped(void *arg, ItemView &view, const KeyEvent &event);
                static void selectionChanged(void *arg, ItemView &view);
                static void edited(void *arg, bool accepted, StringView text);

                Error rebuild(int current, bool blank); // the model from the text (and a blank last row)
                void  act(Action action);
                void  startEdit(size_t row, bool adding);
                void  commit(String &next, int current); // the new text taken, shown, told
                void  updateButtons();

                ListItemModel _model;
                DetailsView  *_view = nullptr;
                Button       *_buttons[ActionCount] = {};
                PopupEdit    *_popup = nullptr;
                String        _text{MemSpace::Psram};
                char          _title[64] = {};
                Changed       _changed = nullptr;
                void         *_changedArg = nullptr;
                AddAsked      _addAsked = nullptr;
                void         *_addAskedArg = nullptr;
                size_t        _editRow = 0;
                char          _separator;
                bool          _adding = false; // the row edited is the blank one Add made
};

BMDS_NAMESPACE_END
