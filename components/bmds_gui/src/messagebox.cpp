/**
 * @file      messagebox.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A standard message for the user (devplan/gui.md §4q).
 */

#include <cstring>
#include <new>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/lineeditmodel.h>
#include <bmds/memory.h>
#include <bmds/messagebox.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Margin = 12;
        constexpr uint16_t IconSide = 32;
        constexpr uint16_t Gap = 12; // the icon from the text, the text from the buttons
        constexpr uint16_t ButtonMin = 76;
        constexpr uint16_t ButtonSpacing = 8;
        constexpr uint16_t MinTextWidth = 200;

        struct Look {
                        IconId      icon;
                        uint32_t    badge; // drawn when the theme has no such icon
                        const char *mark;
        };

        constexpr Look Looks[] = {
                {IconId::DialogInformation, 0x2060c0, "i"},
                {IconId::DialogWarning, 0xd0a000, "!"},
                {IconId::DialogError, 0xc02020, "\xc3\x97"}, // ×
                {IconId::DialogQuestion, 0x2060c0, "?"},
        };

        const char *label(MessageBox::Answer a) {
                switch (a) {
                        case MessageBox::Answer::Ok: return "&OK";
                        case MessageBox::Answer::Cancel: return "&Cancel";
                        case MessageBox::Answer::Yes: return "&Yes";
                        case MessageBox::Answer::No: return "&No";
                }
                return "";
        }

} // namespace

Error MessageBox::show(GuiApplication &app, Kind kind, StringView title, StringView text, Buttons buttons, Done fn,
                       void *arg) {
        // Its text first: the window's size comes from it.
        char   kept[MaxText + 1];
        size_t n = text.size() < MaxText ? text.size() : MaxText;
        while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
        std::memcpy(kept, text.data(), n);
        kept[n] = '\0';
        StringView   lines[MaxLines];
        const size_t count = TextWrap::wrap(StringView(kept, n), MaxColumns, Span<StringView>(lines, MaxLines));
        const Font  &font = Fonts::unscii16;
        uint16_t     textWidth = MinTextWidth;
        for (size_t i = 0; i < count; ++i) {
                const uint32_t w = font.textWidth(lines[i]);
                if (w > textWidth) textWidth = static_cast<uint16_t>(w);
        }
        const uint16_t textHeight = static_cast<uint16_t>(count * GuiStyle::TextHeight);
        Answer         answers[MaxButtons];
        size_t         buttonCount = 0;
        switch (buttons) {
                case Buttons::Ok: answers[buttonCount++] = Answer::Ok; break;
                case Buttons::OkCancel:
                        answers[buttonCount++] = Answer::Ok;
                        answers[buttonCount++] = Answer::Cancel;
                        break;
                case Buttons::YesNo:
                        answers[buttonCount++] = Answer::Yes;
                        answers[buttonCount++] = Answer::No;
                        break;
                case Buttons::YesNoCancel:
                        answers[buttonCount++] = Answer::Yes;
                        answers[buttonCount++] = Answer::No;
                        answers[buttonCount++] = Answer::Cancel;
                        break;
        }
        uint16_t buttonsWidth = 0;
        uint16_t widths[MaxButtons];
        for (size_t i = 0; i < buttonCount; ++i) {
                char           shown[16];
                const size_t   len = Mnemonic::strip(StringView(label(answers[i])), Span<char>(shown)).length;
                const uint32_t w = font.textWidth(StringView(shown, len)) + 2 * GuiStyle::ButtonPad;
                widths[i] = static_cast<uint16_t>(w > ButtonMin ? w : ButtonMin);
                buttonsWidth = static_cast<uint16_t>(buttonsWidth + widths[i] + (i > 0 ? ButtonSpacing : 0));
        }
        const uint16_t bodyHeight = textHeight > IconSide ? textHeight : IconSide;
        uint16_t       clientWidth = static_cast<uint16_t>(Margin + IconSide + Gap + textWidth + Margin);
        if (clientWidth < buttonsWidth + 2 * Margin) clientWidth = static_cast<uint16_t>(buttonsWidth + 2 * Margin);
        clientWidth = static_cast<uint16_t>((clientWidth + 3) & ~3u);
        const uint16_t clientHeight =
                static_cast<uint16_t>(Margin + bodyHeight + Gap + GuiStyle::ButtonHeight + Margin);
        constexpr uint8_t Decorations = WindowFrame::Border | WindowFrame::TitleBar | WindowFrame::CloseButton;
        const uint16_t    width = WindowFrame::outerWidth(clientWidth, Decorations);
        const uint16_t    height = WindowFrame::outerHeight(clientHeight, Decorations);
        const Rect        area = app.windowManager().workArea();
        Config            config;
        config.rect = Rect{uint16_t((area.x + (area.width > width ? (area.width - width) / 2 : 0)) & ~3u),
                           uint16_t(area.y + (area.height > height ? (area.height - height) / 2 : 0)), width, height};
        config.decorations = Decorations;
        config.level = WindowManager::Level::Above;
        config.kind = "message";
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(MessageBox));
        if (mem == nullptr) return Error::NoMem;
        auto *box = ::new (mem) MessageBox(app, config);
        std::memcpy(box->_text, kept, n + 1);
        box->_lineCount =
                TextWrap::wrap(StringView(box->_text, n), MaxColumns, Span<StringView>(box->_lines, MaxLines));
        box->_kind = kind;
        box->_buttons = buttons;
        box->_done = fn;
        box->_doneArg = arg;
        const Look &look = Looks[static_cast<size_t>(kind)];
        const Icon *icon = app.icons().find(look.icon, IconSide);
        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                const size_t pixels = size_t(icon->width) * icon->height;
                box->_iconPixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, pixels * sizeof(uint32_t)));
                if (box->_iconPixels != nullptr) {
                        std::memcpy(box->_iconPixels, icon->pixels, pixels * sizeof(uint32_t));
                        box->_iconWidth = icon->width;
                        box->_iconHeight = icon->height;
                }
        }
        box->_iconRect = Rect{Margin, Margin, IconSide, IconSide};
        box->_textRect = Rect{uint16_t((Margin + IconSide + Gap + 3) & ~3u),
                              uint16_t(Margin + (bodyHeight - textHeight) / 2), textWidth, textHeight};
        // The buttons at the bottom right.
        uint16_t       x = static_cast<uint16_t>(clientWidth - Margin - buttonsWidth);
        const uint16_t y = static_cast<uint16_t>(Margin + bodyHeight + Gap);
        for (size_t i = 0; i < buttonCount; ++i) {
                Button *b = Widget::create<Button>(box, StringView(label(answers[i])));
                if (b == nullptr) break;
                b->setGeometry(Rect{uint16_t(x & ~3u), y, widths[i], GuiStyle::ButtonHeight});
                b->setClicked(clicked, box);
                b->setCentered(true);
                box->_buttonWidgets[box->_buttonCount] = b;
                box->_answers[box->_buttonCount++] = answers[i];
                x = static_cast<uint16_t>(x + widths[i] + ButtonSpacing);
        }
        // The first button the default, with the focus (§4r).
        if (box->_buttonCount > 0) {
                box->setDefaultButton(box->_buttonWidgets[0]);
                box->setFocusWidget(box->_buttonWidgets[0]);
        }
        // The title, then shown.
        Error err = box->GuiWindow::show(); // its own, not the static one
        if (err.isOk() && !title.isEmpty()) (void)app.windowManager().setTitle(box->id(), title);
        if (err.isError()) {
                box->~MessageBox();
                Memory::free(box);
        }
        return err;
}

MessageBox::~MessageBox() {
        Memory::free(_iconPixels);
}

void MessageBox::clicked(void *arg, Button &button) {
        auto *self = static_cast<MessageBox *>(arg);
        for (size_t i = 0; i < self->_buttonCount; ++i)
                if (self->_buttonWidgets[i] == &button) self->answer(self->_answers[i]);
        return;
}

MessageBox::Answer MessageBox::cancelling() const {
        switch (_buttons) {
                case Buttons::Ok: return Answer::Ok;
                case Buttons::OkCancel:
                case Buttons::YesNoCancel: return Answer::Cancel;
                case Buttons::YesNo: return Answer::No;
        }
        return Answer::Cancel;
}

// Told once, then closed and freed after this event.
void MessageBox::answer(Answer answer) {
        if (_answered) return;
        _answered = true;
        close();
        if (_done != nullptr) _done(_doneArg, answer);
        (void)application().post(destroy, this);
        return;
}

void MessageBox::destroy(void *arg) {
        auto *self = static_cast<MessageBox *>(arg);
        self->~MessageBox();
        Memory::free(self);
        return;
}

void MessageBox::closedByManager() {
        answer(cancelling()); // the close box
        return;
}

// Enter is the default button's, Tab the window's; Left and Right move between the buttons too.
bool MessageBox::keyPressed(const KeyEvent &event) {
        if (event.usage == HidKey::Left || event.usage == HidKey::Right) {
                (void)focusNext(event.usage == HidKey::Right);
                return true;
        }
        if (event.usage == HidKey::Escape) {
                answer(cancelling());
                return true;
        }
        return false;
}

void MessageBox::paint(Painter &painter) {
        const WindowPalette &palette = painter.palette();
        const uint32_t       face = palette.color(Role::Face);
        (void)painter.fill(geometry(), face);
        if (_iconPixels != nullptr) {
                (void)painter.image(_iconRect.x, _iconRect.y, Icon{_iconWidth, _iconHeight, _iconPixels}, face);
        } else {
                // No icon in the theme: a badge with its mark.
                const Look &look = Looks[static_cast<size_t>(_kind)];
                (void)painter.fill(_iconRect, look.badge);
                const uint32_t w = Fonts::unscii16.textWidth(StringView(look.mark));
                (void)painter.text(uint16_t((_iconRect.x + (IconSide - w) / 2) & ~3u),
                                   uint16_t(_iconRect.y + (IconSide - GuiStyle::TextHeight) / 2), StringView(look.mark),
                                   Fonts::unscii16, 0xffffff, look.badge, IconSide);
        }
        const uint32_t text = palette.color(Role::FaceText);
        for (size_t i = 0; i < _lineCount; ++i)
                (void)painter.text(_textRect.x, uint16_t(_textRect.y + i * GuiStyle::TextHeight), _lines[i],
                                   Fonts::unscii16, text, face, _textRect.width);
        return;
}

BMDS_NAMESPACE_END
