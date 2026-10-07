/**
 * @file      popupedit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A line edited in place (devplan/gui.md §4q).
 */

#include <cstring>
#include <bmds/guiapplication.h>
#include <bmds/popupedit.h>

BMDS_NAMESPACE_BEGIN

namespace {

        GuiWindow::Config editConfig() {
                GuiWindow::Config c;
                c.title = "Edit";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Popup; // the keys while open; a press elsewhere closes it
                c.focusable = false;
                c.kind = "edit";
                c.pointer = WindowManager::Pointer::Text;
                return c;
        }

} // namespace

PopupEdit::PopupEdit(GuiApplication &app) : GuiWindow(app, editConfig()) {}

Error PopupEdit::open(const Rect &screen, StringView text, Look look, Done fn, void *arg) {
        using Role = WindowPalette::Role;
        if (_edit == nullptr) {
                _edit = Widget::create<LineEdit>(this);
                if (_edit == nullptr) return Error::NoMem;
                _edit->setAccepted(accepted, this);
                _edit->setCancelled(cancelled, this);
                _edit->setLinesChanged(linesChanged, this);
                setCentral(_edit);
                setFocusWidget(_edit);
        }
        _done = fn;
        _doneArg = arg;
        _look = look;
        const bool name = look == Look::IconName;
        _edit->setFramed(!name);
        _edit->setColors(name ? Role::Desktop : Role::FaceHighlight, name ? Role::DesktopText : Role::FaceText);
        _edit->setCentered(name);
        _edit->setWrapping(name);
        _edit->setMaxLength(LineEditModel::MaxBytes);
        // Its lines counted at its width before it opens, so it opens as tall as they are.
        const uint16_t width = static_cast<uint16_t>((screen.width + 3) & ~3u);
        _edit->setGeometry(Rect{0, 0, width, GuiStyle::TextHeight});
        _edit->setText(text);
        _edit->selectAll();
        const Error err =
                show(Rect{uint16_t(screen.x & ~3u), screen.y, width, heightFor(name ? _edit->lineCount() : 1)});
        _open = err.isOk();
        return err;
}

uint16_t PopupEdit::heightFor(size_t lines) const {
        if (_look == Look::Field) return GuiStyle::ButtonHeight;
        if (lines > MaxLines) lines = MaxLines;
        return static_cast<uint16_t>((lines > 0 ? lines : 1) * GuiStyle::TextHeight);
}

// The text wrapped onto more or fewer lines: as tall as they are, its top where it was.
void PopupEdit::linesChanged(void *arg, LineEdit &, size_t lines) {
        auto *self = static_cast<PopupEdit *>(arg);
        if (!self->_open || self->_look != Look::IconName) return;
        const uint16_t height = self->heightFor(lines);
        if (height != self->geometry().height)
                (void)self->application().windowManager().resize(self->id(), self->geometry().width, height);
        return;
}

void PopupEdit::accepted(void *arg, LineEdit &) {
        static_cast<PopupEdit *>(arg)->finish(true);
        return;
}

void PopupEdit::cancelled(void *arg, LineEdit &) {
        static_cast<PopupEdit *>(arg)->finish(false);
        return;
}

// A press elsewhere closed it: as Enter does, it keeps what was typed.
void PopupEdit::closedByManager() {
        finish(true);
        return;
}

void PopupEdit::finish(bool accept) {
        if (!_open) return;
        _open = false;
        close();
        // The text copied first: the callback may open it again with other text.
        char             text[LineEditModel::MaxBytes + 1];
        const StringView t = _edit->text();
        std::memcpy(text, t.data(), t.size());
        text[t.size()] = '\0';
        if (_done != nullptr) _done(_doneArg, accept, StringView(text, t.size()));
        return;
}

BMDS_NAMESPACE_END
