/**
 * @file      listeditor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An ordered list of texts to edit (devplan/gui.md §4u).
 */

#include <cstring>
#include <new>
#include <bmds/detailsview.h>
#include <bmds/formmodels.h>
#include <bmds/guiwindow.h>
#include <bmds/lineedit.h>
#include <bmds/listeditor.h>
#include <bmds/memory.h>
#include <bmds/popupedit.h>
#include <bmds/scrollarea.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ButtonText[] = {"Add", "Edit", "Remove", "Up", "Down"};
        constexpr uint16_t    ButtonGap = 4;
        constexpr uint16_t    ListGap = 8; // the list to the buttons

} // namespace

ListEditor::ListEditor(Widget *parent, char separator) : Widget(parent), _separator(separator) {
        _view = Widget::create<DetailsView>(this);
        if (_view != nullptr) {
                _view->setSingleSelection(true);
                _view->setModel(&_model);
                _view->setActivated(activated, this);
                _view->setRenameAsked(renameAsked, this);
                _view->setKeyTyped(keyTyped, this);
                _view->setChanged(selectionChanged, this);
        }
        for (size_t i = 0; i < ActionCount; ++i) {
                _buttons[i] = Widget::create<Button>(this, ButtonText[i]);
                if (_buttons[i] == nullptr) continue;
                _buttons[i]->setCentered(true);
                _buttons[i]->setData(i);
                _buttons[i]->setClicked(clicked, this);
        }
        setTitle(StringView());
        updateButtons();
}

ListEditor::~ListEditor() {
        if (_popup != nullptr) {
                _popup->~PopupEdit();
                Memory::free(_popup);
        }
}

Error ListEditor::setText(StringView text) {
        if (Error e = _text.assign(text); e.isError()) return e;
        return rebuild(count() > 0 ? 0 : -1, false);
}

size_t ListEditor::count() const {
        return ListText::count(_text.view(), _separator);
}

StringView ListEditor::item(size_t index) const {
        return ListText::item(_text.view(), _separator, index);
}

void ListEditor::setTitle(StringView title) {
        const size_t n = title.size() < sizeof(_title) - 1 ? title.size() : sizeof(_title) - 1;
        std::memcpy(_title, title.data(), n);
        _title[n] = '\0';
        const StringView       titles[1] = {StringView(_title)};
        const uint16_t         widths[1] = {_view != nullptr ? _view->geometry().width : uint16_t(200)};
        const ItemModel::Align aligns[1] = {ItemModel::Align::Left};
        _model.setColumns(Span<const StringView>(titles), Span<const uint16_t>(widths),
                          Span<const ItemModel::Align>(aligns));
        if (_view != nullptr) _view->setHeaderVisible(_title[0] != '\0'); // no title: no header
        (void)rebuild(_view != nullptr ? _view->current() : -1, false);   // the selection kept
        return;
}

Error ListEditor::addItem(StringView text) {
        if (text.isEmpty()) return Error::Ok;
        const size_t n = count();
        String       next(MemSpace::Psram);
        if (Error e = ListText::insert(_text.view(), _separator, n, text, next); e.isError()) return e;
        commit(next, int(n));
        return Error::Ok;
}

Error ListEditor::rebuild(int current, bool blank) {
        _model.clear();
        const size_t n = count();
        for (size_t i = 0; i < n; ++i)
                if (Error e = _model.add(item(i), nullptr, nullptr); e.isError()) return e;
        if (blank)
                if (Error e = _model.add(StringView(), nullptr, nullptr); e.isError()) return e;
        if (_view != nullptr) {
                const size_t rows = n + (blank ? 1 : 0);
                if (current >= int(rows)) current = int(rows) - 1;
                const size_t selected = current >= 0 ? size_t(current) : 0;
                _view->resetTo(Span<const size_t>(&selected, current >= 0 ? 1 : 0), current);
                if (current >= 0) _view->showRow(size_t(current));
        }
        updateButtons();
        return Error::Ok;
}

void ListEditor::commit(String &next, int current) {
        _text = static_cast<String &&>(next);
        (void)rebuild(current, false);
        if (_changed != nullptr) _changed(_changedArg, *this);
        return;
}

void ListEditor::updateButtons() {
        const int    cur = _view != nullptr ? _view->current() : -1;
        const size_t n = count();
        const bool   on[ActionCount] = {true, cur >= 0 && size_t(cur) < n, cur >= 0 && size_t(cur) < n,
                                        cur > 0 && size_t(cur) < n, cur >= 0 && size_t(cur) + 1 < n};
        for (size_t i = 0; i < ActionCount; ++i)
                if (_buttons[i] != nullptr) _buttons[i]->setEnabled(on[i]);
        return;
}

void ListEditor::act(Action action) {
        const int    cur = _view != nullptr ? _view->current() : -1;
        const size_t n = count();
        String       next(MemSpace::Psram);
        switch (action) {
                case Add:
                        if (_addAsked != nullptr) {
                                _addAsked(_addAskedArg, *this);
                        } else if (rebuild(int(n), true).isOk()) {
                                startEdit(n, true);
                        }
                        break;
                case Edit:
                        if (cur >= 0 && size_t(cur) < n) startEdit(size_t(cur), false);
                        break;
                case Remove:
                        if (cur >= 0 && ListText::remove(_text.view(), _separator, size_t(cur), next).isOk())
                                commit(next, cur);
                        break;
                case Up:
                        if (cur > 0 &&
                            ListText::move(_text.view(), _separator, size_t(cur), size_t(cur - 1), next).isOk())
                                commit(next, cur - 1);
                        break;
                case Down:
                        if (cur >= 0 && size_t(cur) + 1 < n &&
                            ListText::move(_text.view(), _separator, size_t(cur), size_t(cur + 1), next).isOk())
                                commit(next, cur + 1);
                        break;
                default: break;
        }
        return;
}

void ListEditor::startEdit(size_t row, bool adding) {
        GuiWindow *w = window();
        if (w == nullptr || _view == nullptr) return;
        if (_popup == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(PopupEdit));
                if (mem == nullptr) return;
                _popup = ::new (mem) PopupEdit(w->application());
        }
        _editRow = row;
        _adding = adding;
        _view->showRow(row);
        if (_popup->open(_view->nameRect(row), adding ? StringView() : item(row), PopupEdit::Look::Field, edited, this)
                    .isError()) {
                if (adding) (void)rebuild(row > 0 ? int(row) - 1 : -1, false);
                return;
        }
        _popup->edit()->setMaxLength(MaxItem);
        _view->setEditing(int(row));
        return;
}

void ListEditor::edited(void *arg, bool accepted, StringView text) {
        auto *self = static_cast<ListEditor *>(arg);
        if (self->_view != nullptr) {
                self->_view->setEditing(-1);
                self->_view->setFocus();
        }
        const size_t n = self->count();
        String       next(MemSpace::Psram);
        if (self->_adding) {
                self->_adding = false;
                if (accepted && !text.isEmpty() &&
                    ListText::insert(self->_text.view(), self->_separator, n, text, next).isOk())
                        self->commit(next, int(n));
                else
                        (void)self->rebuild(n > 0 ? int(n) - 1 : -1, false); // the blank row goes
                return;
        }
        const size_t row = self->_editRow;
        if (!accepted || row >= n || text == self->item(row)) return;
        const Error e = text.isEmpty() ? ListText::remove(self->_text.view(), self->_separator, row, next)
                                       : ListText::replace(self->_text.view(), self->_separator, row, text, next);
        if (e.isOk()) self->commit(next, int(row));
        return;
}

void ListEditor::clicked(void *arg, Button &button) {
        static_cast<ListEditor *>(arg)->act(static_cast<Action>(button.data()));
        return;
}

void ListEditor::activated(void *arg, ItemView &, size_t) {
        static_cast<ListEditor *>(arg)->act(Edit);
        return;
}

void ListEditor::renameAsked(void *arg, ItemView &, size_t row) {
        auto *self = static_cast<ListEditor *>(arg);
        if (row < self->count()) self->startEdit(row, false);
        return;
}

bool ListEditor::keyTyped(void *arg, ItemView &, const KeyEvent &event) {
        auto *self = static_cast<ListEditor *>(arg);
        if (event.action == KeyEvent::Action::Release || event.hasCtrl()) return false;
        if (event.hasAlt() && event.usage == HidKey::Up) {
                self->act(Up);
        } else if (event.hasAlt() && event.usage == HidKey::Down) {
                self->act(Down);
        } else if (!event.hasAlt() && event.usage == HidKey::Insert) {
                self->act(Add);
        } else if (!event.hasAlt() && event.usage == HidKey::Delete) {
                self->act(Remove);
        } else {
                return false;
        }
        return true;
}

void ListEditor::selectionChanged(void *arg, ItemView &) {
        static_cast<ListEditor *>(arg)->updateButtons();
        return;
}

GuiSize ListEditor::sizeHint() const {
        uint16_t buttons = 0;
        for (const Button *b : _buttons)
                if (b != nullptr && b->sizeHint().width > buttons) buttons = b->sizeHint().width;
        // The list's header and rows in its font (as DetailsView's: its ink box and 4, and 2).
        const uint32_t listHigh = uint32_t(textHeight() + 4) + ShownRows * uint32_t(textHeight() + 2) + 4;
        const uint32_t buttonsHigh = ActionCount * buttonHeight() + (ActionCount - 1) * ButtonGap;
        return GuiSize{static_cast<uint16_t>(200 + ListGap + buttons),
                       static_cast<uint16_t>(listHigh > buttonsHigh ? listHigh : buttonsHigh)};
}

void ListEditor::resized() {
        const Rect &r = geometry();
        uint16_t    bw = 0;
        for (const Button *b : _buttons)
                if (b != nullptr && b->sizeHint().width > bw) bw = b->sizeHint().width;
        bw = static_cast<uint16_t>((bw + GuiStyle::Column - 1) & ~(GuiStyle::Column - 1));
        const uint16_t listWidth = r.width > bw + ListGap ? static_cast<uint16_t>(r.width - bw - ListGap) : 0;
        if (_view != nullptr) {
                _view->setGeometry(Rect{r.x, r.y, listWidth, r.height});
                // The one column across the list, less its bar and edges.
                const uint16_t inner =
                        listWidth > ScrollBar::Breadth + 8 ? uint16_t(listWidth - ScrollBar::Breadth - 8) : 0;
                _view->setColumnWidth(0, inner);
        }
        uint32_t y = r.y;
        for (Button *b : _buttons) {
                if (b == nullptr) continue;
                b->setGeometry(Rect{uint16_t(r.right() - bw), uint16_t(y), bw, buttonHeight()});
                y += buttonHeight() + ButtonGap;
        }
        return;
}

void ListEditor::paint(Painter &painter) {
        fillOutsideChildren(painter, geometry(), painter.palette().color(WindowPalette::Role::Face));
        return;
}

BMDS_NAMESPACE_END
