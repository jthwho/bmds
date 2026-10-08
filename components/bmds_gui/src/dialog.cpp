/**
 * @file      dialog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A window asking something (devplan/filebrowser.md §4e).
 */

#include <bmds/dialog.h>
#include <bmds/font.h>
#include <bmds/furniturelayout.h>
#include <bmds/guiapplication.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Gap = 12; // the content from the buttons
        constexpr uint8_t  Decorations = WindowFrame::Border | WindowFrame::TitleBar | WindowFrame::CloseButton;
        constexpr uint16_t Spacing = 8; // between the content's rows

        GuiWindow::Config configFor() {
                GuiWindow::Config c;
                c.decorations = Decorations;
                c.level = WindowManager::Level::Above;
                c.kind = "dialog";
                return c;
        }

        uint16_t textWidthOf(const Button &b) {
                return static_cast<uint16_t>(Fonts::unscii16.textWidth(b.text()));
        }

} // namespace

Dialog::Dialog(GuiApplication &app, StringView title, uint16_t width, uint16_t height)
    : GuiWindow(app, configFor()), _width(static_cast<uint16_t>((width + 3u) & ~3u)), _height(height) {
        size_t n = title.size() < WindowManager::MaxTitle ? title.size() : WindowManager::MaxTitle;
        while (n > 0 && n < title.size() && (uint8_t(title[n]) & 0xc0) == 0x80) --n;
        setTitle(title.left(n)); // the window's: setTitle() changes it, open or not
        _content = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (_content != nullptr) _content->setSpacing(Spacing);
}

Button *Dialog::addButton(StringView text, int result) {
        if (_count == MaxButtons) return nullptr;
        auto *b = Widget::create<Button>(this, text);
        if (b == nullptr) return nullptr;
        b->setClicked(clicked, this);
        b->setCentered(true);
        _buttons[_count] = b;
        _results[_count++] = result;
        if (_count == 1) {
                setDefaultButton(b);
                setFocusWidget(b);
        }
        resized();
        return b;
}

int Dialog::cancelResult() const {
        if (_cancelSet || _count == 0) return _cancel;
        return _results[_count - 1];
}

Error Dialog::open() {
        uint16_t widths[MaxButtons];
        for (size_t i = 0; i < _count; ++i) widths[i] = textWidthOf(*_buttons[i]);
        const uint16_t row = ButtonRow::width(Span<const uint16_t>(widths, _count));
        uint16_t       clientWidth = static_cast<uint16_t>(_width + 2 * Margin);
        if (clientWidth < row + 2 * Margin) clientWidth = static_cast<uint16_t>(row + 2 * Margin);
        clientWidth = static_cast<uint16_t>((clientWidth + 3u) & ~3u);
        const uint16_t clientHeight =
                static_cast<uint16_t>(Margin + _height + (_count > 0 ? Gap + GuiStyle::ButtonHeight : 0) + Margin);
        const uint16_t width = WindowFrame::outerWidth(clientWidth, Decorations);
        const uint16_t height = WindowFrame::outerHeight(clientHeight, Decorations);
        const Rect     area = application().windowManager().workArea();
        const Rect     at{uint16_t((area.x + (area.width > width ? (area.width - width) / 2 : 0)) & ~3u),
                      uint16_t(area.y + (area.height > height ? (area.height - height) / 2 : 0)), width, height};
        _finishedOnce = false;
        const Error err = show(at);
        if (err.isOk()) activate();
        return err;
}

void Dialog::resized() {
        const Rect &g = geometry();
        if (g.width < 2 * Margin || g.height < 2 * Margin) return;
        const uint16_t buttonsTop = static_cast<uint16_t>(
                g.height > Margin + GuiStyle::ButtonHeight ? g.height - Margin - GuiStyle::ButtonHeight : 0);
        const uint16_t contentBottom =
                _count > 0 && buttonsTop > Gap ? uint16_t(buttonsTop - Gap) : uint16_t(g.height - Margin);
        if (_content != nullptr)
                _content->setGeometry(Rect{Margin, Margin, uint16_t(g.width - 2 * Margin),
                                           uint16_t(contentBottom > Margin ? contentBottom - Margin : 0)});
        uint16_t widths[MaxButtons];
        Rect     places[MaxButtons];
        for (size_t i = 0; i < _count; ++i) widths[i] = textWidthOf(*_buttons[i]);
        ButtonRow::place(Span<const uint16_t>(widths, _count), uint16_t(g.width - Margin), buttonsTop,
                         GuiStyle::ButtonHeight, Span<Rect>(places, _count));
        for (size_t i = 0; i < _count; ++i) _buttons[i]->setGeometry(places[i]);
        update();
        return;
}

void Dialog::finish(int result) {
        if (_finishedOnce) return;
        _finishedOnce = true;
        close();
        if (_finished != nullptr) _finished(_finishedArg, *this, result);
        return;
}

void Dialog::clicked(void *arg, Button &button) {
        auto *self = static_cast<Dialog *>(arg);
        for (size_t i = 0; i < self->_count; ++i)
                if (self->_buttons[i] == &button) self->finish(self->_results[i]);
        return;
}

void Dialog::closedByManager() {
        finish(cancelResult()); // the close box
        return;
}

// Escape cancels; Left and Right move between the buttons (Tab and Enter are the window's).
bool Dialog::keyPressed(const KeyEvent &event) {
        if (event.usage == HidKey::Escape) {
                finish(cancelResult());
                return true;
        }
        if (event.usage == HidKey::Left || event.usage == HidKey::Right) {
                for (size_t i = 0; i < _count; ++i)
                        if (focusWidget() == _buttons[i]) {
                                const size_t next = (i + (event.usage == HidKey::Right ? 1 : _count - 1)) % _count;
                                setFocusWidget(_buttons[next]);
                                return true;
                        }
        }
        return false;
}

void Dialog::paint(Painter &painter) {
        (void)painter.fill(geometry(), painter.palette().color(Role::Face));
        return;
}

BMDS_NAMESPACE_END
