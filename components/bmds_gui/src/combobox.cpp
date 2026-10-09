/**
 * @file      combobox.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A choice of one item from a list (devplan/mediaplayer.md §6d).
 */

#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/combobox.h>
#include <bmds/font.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/menu.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint8_t  F4 = HidKey::F1 + 3;
        constexpr uint16_t TextPad = 4;

} // namespace

ComboBox::ComboBox(Widget *parent) : Widget(parent) {
        setFocusable(true);
}

ComboBox::~ComboBox() {
        if (_menu != nullptr) {
                _menu->~Menu();
                Memory::free(_menu);
        }
}

Error ComboBox::addItem(StringView text, uintptr_t data) {
        if (_count == MaxItems) return Error::Full;
        Item  &item = _items[_count];
        size_t n = text.size() < MaxText ? text.size() : MaxText;
        while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
        std::memcpy(item.text, text.data(), n);
        item.text[n] = '\0';
        item.data = data;
        if (_count++ == 0) _current = 0;
        update();
        return Error::Ok;
}

void ComboBox::clear() {
        _count = 0;
        _current = -1;
        update();
        return;
}

void ComboBox::setCurrent(int index) {
        if (index < -1 || index >= int(_count) || index == _current) return;
        _current = index;
        update();
        return;
}

int ComboBox::indexOfData(uintptr_t data) const {
        for (size_t i = 0; i < _count; ++i)
                if (_items[i].data == data) return int(i);
        return -1;
}

void ComboBox::userSet(int index) {
        if (index < 0 || index >= int(_count) || index == _current) return;
        _current = index;
        update();
        if (_changed != nullptr) _changed(_changedArg, *this, index);
        return;
}

GuiSize ComboBox::sizeHint() const {
        uint32_t widest = 0;
        for (size_t i = 0; i < _count; ++i) {
                const uint32_t w = Fonts::unscii16.textWidth(StringView(_items[i].text));
                if (w > widest) widest = w;
        }
        const uint32_t width = 2u * Bevel::width(Bevel::Style::Field) + TextPad + widest + TextPad + ArrowWidth;
        return GuiSize{uint16_t(((width < 64 ? 64 : width) + 3u) & ~3u), GuiStyle::ButtonHeight};
}

void ComboBox::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        (void)painter.fill(g, p.color(Role::Face));
        if (g.width < 2 * ArrowWidth || g.height < 8) return;
        drawBevel(painter, g, Bevel::Style::Field);
        const Rect in = Bevel::inside(g, Bevel::Style::Field);
        // The arrow's button at the right, inside the field.
        const Rect arrow{uint16_t(in.right() - ArrowWidth), in.y, ArrowWidth, in.height};
        drawBevel(painter, arrow, Bevel::Style::Raised);
        (void)painter.fill(Bevel::inside(arrow, Bevel::Style::Raised), p.color(Role::Face));
        GuiStyle::drawArrow(painter, arrow, GuiStyle::Arrow::Down,
                            p.color(isEnabled() ? Role::FaceText : Role::DisabledText));
        // The text, highlighted while it has the focus.
        const Rect     field{in.x, in.y, uint16_t(in.width - ArrowWidth), in.height};
        const bool     focused = hasFocus() && window() != nullptr && window()->isActive();
        const uint32_t bg = p.color(focused ? Role::Highlight : Role::FaceHighlight);
        const uint32_t fg = p.color(!isEnabled() ? Role::DisabledText : focused ? Role::HighlightText : Role::FaceText);
        (void)painter.fill(field, p.color(Role::FaceHighlight));
        if (focused && field.width > 2 && field.height > 2)
                (void)painter.fill(Rect{uint16_t(field.x + 1), uint16_t(field.y + 1), uint16_t(field.width - 2),
                                        uint16_t(field.height - 2)},
                                   bg);
        const int x = (field.x + TextPad + 3) & ~3;
        const int room = int(field.right()) - x - 2;
        if (_current >= 0 && room > 0)
                (void)painter.text(x, field.y + (field.height - GuiStyle::TextHeight) / 2,
                                   StringView(_items[_current].text), Fonts::unscii16, fg, bg, uint16_t(room));
        return;
}

void ComboBox::open() {
        GuiWindow *w = window();
        if (w == nullptr || !w->isShown() || _count == 0) return;
        if (_menu == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) return;
                _menu = ::new (mem) Menu(w->application());
                _menu->setTriggered(chosen, this);
        }
        _menu->clear();
        for (size_t i = 0; i < _count; ++i) {
                // `&` is the menu's mark: doubled to show as itself.
                char        text[2 * MaxText + 1];
                size_t      n = 0;
                const char *s = _items[i].text;
                for (; *s != '\0' && n + 2 < sizeof(text); ++s) {
                        if (*s == '&') text[n++] = '&';
                        text[n++] = *s;
                }
                (void)_menu->addItem(StringView(text, n), int(i), int(i) == _current ? MenuLayout::Checked : 0);
        }
        const Rect &g = geometry();
        const Rect  s = w->screenRect();
        if (_menu->popup(Rect{uint16_t(s.x + g.x), uint16_t(s.y + g.y), g.width, g.height}).isOk())
                _menu->highlightFirst();
        return;
}

void ComboBox::chosen(void *arg, int item) {
        static_cast<ComboBox *>(arg)->userSet(item);
        return;
}

void ComboBox::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Left) open();
        return;
}

bool ComboBox::wheelTurned(int notches) {
        if (_count == 0) return false;
        int to = _current - notches; // up: toward the first
        if (to < 0) to = 0;
        if (to >= int(_count)) to = int(_count) - 1;
        userSet(to);
        return true;
}

bool ComboBox::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || event.hasCtrl()) return false;
        if ((event.hasAlt() && (event.usage == HidKey::Down || event.usage == HidKey::Up)) ||
            (event.usage == F4 && !event.hasAlt())) {
                open();
                return true;
        }
        if (event.hasAlt() || _count == 0) return false;
        int to = _current;
        switch (event.usage) {
                case HidKey::Up:
                case HidKey::Left: to = _current > 0 ? _current - 1 : 0; break;
                case HidKey::Down:
                case HidKey::Right: to = _current + 1 < int(_count) ? _current + 1 : int(_count) - 1; break;
                case HidKey::Home: to = 0; break;
                case HidKey::End: to = int(_count) - 1; break;
                default: return false;
        }
        userSet(to);
        return true;
}

BMDS_NAMESPACE_END
