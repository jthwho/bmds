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

Error PopupEdit::open(const Rect &screen, StringView text, Done fn, void *arg) {
        if (_edit == nullptr) {
                _edit = Widget::create<LineEdit>(this);
                if (_edit == nullptr) return Error::NoMem;
                _edit->setAccepted(accepted, this);
                _edit->setCancelled(cancelled, this);
                setCentral(_edit);
                setFocusWidget(_edit);
        }
        _done = fn;
        _doneArg = arg;
        _edit->setText(text);
        _edit->selectAll();
        const Error err = show(
                Rect{uint16_t(screen.x & ~3u), screen.y, uint16_t((screen.width + 3) & ~3u), GuiStyle::ButtonHeight});
        _open = err.isOk();
        return err;
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
