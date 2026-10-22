/**
 * @file      findbar.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Find, replace and go to a line in a TextEdit (devplan/editor.md §2).
 */

#include <bmds/findbar.h>

#include <cstdio>
#include <bmds/lineedit.h>
#include <bmds/textedit.h>

BMDS_NAMESPACE_BEGIN

namespace {

        enum Action : uintptr_t {
                ActNext = 1,
                ActPrevious,
                ActReplace,
                ActAll,
                ActClose,
        };

        constexpr uint8_t F3 = HidKey::F1 + 2;

} // namespace

FindBar::FindBar(Widget *parent) : BoxLayout(parent, Direction::Vertical) {
        setMargins(4, 2, 4, 2);
        setSpacing(2);
        _findRow = Widget::create<BoxLayout>(this, Direction::Horizontal);
        _replaceRow = Widget::create<BoxLayout>(this, Direction::Horizontal);
        if (_findRow == nullptr || _replaceRow == nullptr) return;
        _findRow->setSpacing(4);
        _replaceRow->setSpacing(4);
        _findLabel = Widget::create<Label>(_findRow, "Find:");
        _find = Widget::create<LineEdit>(_findRow);
        _case = Widget::create<CheckBox>(_findRow, "Match &case");
        _word = Widget::create<CheckBox>(_findRow, "&Whole word");
        _next = Widget::create<Button>(_findRow, "&Next");
        _previous = Widget::create<Button>(_findRow, "&Previous");
        _status = Widget::create<Label>(_findRow);
        _closeButton = Widget::create<Button>(_findRow, "Close");
        auto *replaceLabel = Widget::create<Label>(_replaceRow, "Replace:");
        _replace = Widget::create<LineEdit>(_replaceRow);
        _replaceButton = Widget::create<Button>(_replaceRow, "&Replace");
        _allButton = Widget::create<Button>(_replaceRow, "Replace &All");
        if (_findLabel == nullptr || _find == nullptr || _case == nullptr || _word == nullptr || _next == nullptr ||
            _previous == nullptr || _status == nullptr || _closeButton == nullptr || replaceLabel == nullptr ||
            _replace == nullptr || _replaceButton == nullptr || _allButton == nullptr)
                return;
        // The labels as wide as each other, so the fields line up.
        _findLabel->setMinimumWidth(72);
        replaceLabel->setMinimumWidth(72);
        _find->setStretch(1);
        _replace->setStretch(1);
        _find->setMaximumWidth(320);
        _replace->setMaximumWidth(320);
        _find->setAccepted(accepted, this);
        _find->setCancelled(cancelled, this);
        _find->setEdited(edited, this);
        _replace->setAccepted(accepted, this);
        _replace->setCancelled(cancelled, this);
        const struct {
                        Button   *button;
                        uintptr_t action;
        } buttons[] = {{_next, ActNext},
                       {_previous, ActPrevious},
                       {_replaceButton, ActReplace},
                       {_allButton, ActAll},
                       {_closeButton, ActClose}};
        for (const auto &b : buttons) {
                b.button->setData(b.action);
                b.button->setClicked(clicked, this);
        }
        setVisible(false);
}

void FindBar::open(Mode mode) {
        if (_find == nullptr) return;
        _mode = mode;
        const bool lines = mode == Mode::GoToLine;
        _findLabel->setText(lines ? "Line:" : "Find:");
        _replaceRow->setVisible(mode == Mode::Replace);
        for (Widget *w : {static_cast<Widget *>(_case), static_cast<Widget *>(_word), static_cast<Widget *>(_previous)})
                w->setVisible(!lines);
        _next->setText(lines ? "&Go" : "&Next");
        say(StringView());
        if (_target != nullptr) {
                if (lines) {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%u", unsigned(_target->selection().cursor.line + 1));
                        _find->setText(text);
                } else {
                        String          selected(MemSpace::Psram);
                        const TextRange r = _target->selection().range();
                        if (!r.isEmpty() && r.start.line == r.end.line && _target->selectedText(selected).isOk())
                                _find->setText(selected.view());
                }
        }
        _findRow->relayout();
        _replaceRow->relayout();
        setVisible(true);
        relayout();
        if (_toggled != nullptr) _toggled(_toggledArg, *this);
        _find->selectAll();
        _find->setFocus();
        return;
}

void FindBar::close() {
        setVisible(false);
        if (_toggled != nullptr) _toggled(_toggledArg, *this);
        if (_target != nullptr) _target->setFocus();
        return;
}

uint32_t FindBar::flags() const {
        uint32_t f = TextDocument::FindWrap;
        if (_case != nullptr && _case->isChecked()) f |= TextDocument::FindCaseSensitive;
        if (_word != nullptr && _word->isChecked()) f |= TextDocument::FindWholeWord;
        return f;
}

void FindBar::say(StringView text) {
        if (_status == nullptr) return;
        _status->setText(text);
        if (_findRow != nullptr) _findRow->relayout(); // it takes the room its text needs
        return;
}

bool FindBar::findNext(bool backward) {
        if (_target == nullptr || _find == nullptr) return false;
        const StringView needle = _find->text();
        if (needle.isEmpty()) return false;
        const bool found = _target->find(needle, flags() | (backward ? uint32_t(TextDocument::FindBackward) : 0u));
        say(found ? StringView() : StringView("Not found"));
        return found;
}

void FindBar::replaceOne() {
        if (_target == nullptr || _find->text().isEmpty()) return;
        const bool found = _target->replace(_find->text(), _replace->text(), flags());
        say(found ? StringView() : StringView("Not found"));
        return;
}

void FindBar::replaceAll() {
        if (_target == nullptr || _find->text().isEmpty()) return;
        const size_t n = _target->replaceAll(_find->text(), _replace->text(), flags() & ~TextDocument::FindWrap);
        char         text[32];
        std::snprintf(text, sizeof(text), n == 1 ? "1 replaced" : "%u replaced", unsigned(n));
        say(text);
        return;
}

void FindBar::goToLine() {
        if (_target == nullptr) return;
        const StringView text = _find->text();
        uint32_t         line = 0;
        bool             digits = !text.isEmpty();
        for (char c : text) {
                if (c < '0' || c > '9' || line > 100000000u) {
                        digits = false;
                        break;
                }
                line = line * 10 + uint32_t(c - '0');
        }
        if (!digits || line == 0) {
                say("Not a line number");
                return;
        }
        _target->goToLine(line - 1);
        close();
        return;
}

void FindBar::accepted(void *arg, LineEdit &edit) {
        auto *self = static_cast<FindBar *>(arg);
        if (self->_mode == Mode::GoToLine)
                self->goToLine();
        else if (&edit == self->_replace)
                self->replaceOne();
        else
                (void)self->findNext(false);
        return;
}

void FindBar::cancelled(void *arg, LineEdit &) {
        static_cast<FindBar *>(arg)->close();
        return;
}

void FindBar::edited(void *arg, LineEdit &) {
        static_cast<FindBar *>(arg)->say(StringView());
        return;
}

void FindBar::clicked(void *arg, Button &button) {
        auto *self = static_cast<FindBar *>(arg);
        switch (button.data()) {
                case ActNext:
                        if (self->_mode == Mode::GoToLine)
                                self->goToLine();
                        else
                                (void)self->findNext(false);
                        break;
                case ActPrevious: (void)self->findNext(true); break;
                case ActReplace: self->replaceOne(); break;
                case ActAll: self->replaceAll(); break;
                case ActClose: self->close(); break;
                default: break;
        }
        return;
}

// Shift+Enter finds backward from either field; F3 and Shift+F3 work anywhere in it.
bool FindBar::keyBeforeChild(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const bool enter = event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter;
        if ((enter && event.hasShift() && _mode != Mode::GoToLine) || (event.usage == F3 && _mode != Mode::GoToLine)) {
                (void)findNext(event.hasShift());
                return true;
        }
        return BoxLayout::keyBeforeChild(event);
}

BMDS_NAMESPACE_END
