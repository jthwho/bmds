/**
 * @file      menubar.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A window's menu bar (devplan/filebrowser.md §4e).
 */

#include <cstring>
#include <bmds/bevel.h>
#include <bmds/bitmapfont.h>
#include <bmds/furniturelayout.h>
#include <bmds/guiwindow.h>
#include <bmds/menubar.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

} // namespace

/**
 * @brief A menu bar's title: its own widget, for its mnemonic and the pointer.
 */
class MenuTitle final : public Widget {
        public:
                MenuTitle(Widget *parent, MenuBar &bar, size_t index, StringView text)
                    : Widget(parent), _bar(bar), _index(index) {
                        setText(text);
                }

                void setText(StringView text) {
                        const Mnemonic::Stripped m = Mnemonic::strip(text, Span<char>(_text, sizeof(_text)));
                        _underline = m.at;
                        _key = m.key;
                        update();
                        return;
                }

                uint16_t textWidth() const { return static_cast<uint16_t>(font().textWidth(StringView(_text))); }

                void paint(Painter &painter) override {
                        const WindowPalette &p = painter.palette();
                        const Rect          &r = geometry();
                        const bool           open = _bar.openIndex() == int(_index);
                        const uint32_t       face = p.color(Role::Face);
                        (void)painter.fill(r, face);
                        if (open) {
                                drawBevel(painter, r, Bevel::Style::Status);
                        } else if (_over && _bar.openIndex() < 0) {
                                drawBevel(painter, r, Bevel::Style::Thin);
                        }
                        const uint16_t x = static_cast<uint16_t>(r.x + MenuBarLayout::Pad);
                        const uint16_t y = static_cast<uint16_t>(r.y + (r.height - textHeight()) / 2 + (open ? 1 : 0));
                        const uint32_t fg = p.color(isEnabled() ? Role::FaceText : Role::DisabledText);
                        (void)painter.text(x, y, StringView(_text), font(), fg, face, textWidth());
                        Mnemonic::draw(painter, x, y, StringView(_text), _underline, fg, x + textWidth(), font());
                        return;
                }

                void mousePressed(const WindowManager::MouseEvent &event) override {
                        if (event.button == PointerEvent::Left) (void)_bar.open(_index, false);
                        return;
                }

                void mouseMoved(const WindowManager::MouseEvent &event) override {
                        (void)event;
                        if (!_over) {
                                _over = true;
                                update();
                        }
                        _bar.hovered(_index);
                        return;
                }

                void mouseLeft() override {
                        _over = false;
                        update();
                        return;
                }

                uint32_t mnemonic() const override { return _key; }

                void mnemonicActivated() override {
                        (void)_bar.open(_index, true);
                        return;
                }

        private:
                MenuBar &_bar;
                size_t   _index;
                char     _text[MenuBar::MaxText + 1] = {};
                size_t   _underline = Mnemonic::None;
                uint32_t _key = 0;
                bool     _over = false;
};

MenuBar::MenuBar(Widget *parent) : Widget(parent) {}

Error MenuBar::addMenu(StringView title, Menu &menu) {
        if (_count == MaxMenus) return Error::Full;
        auto *t = Widget::create<MenuTitle>(this, *this, _count, title);
        if (t == nullptr) return Error::NoMem;
        _titles[_count] = t;
        _menus[_count] = &menu;
        ++_count;
        menu.setDismissed(dismissed, this);
        menu.setMoved(moved, this);
        resized();
        return Error::Ok;
}

void MenuBar::setTitle(size_t index, StringView title) {
        if (index >= _count) return;
        _titles[index]->setText(title);
        resized();
        update();
        return;
}

void MenuBar::clear() {
        if (_open >= 0 && _menus[_open] != nullptr) _menus[_open]->dismiss();
        for (size_t i = 0; i < _count; ++i) {
                _menus[i]->setDismissed(nullptr, nullptr);
                _menus[i]->setMoved(nullptr, nullptr);
                _titles[i]->destroy();
                _titles[i] = nullptr;
                _menus[i] = nullptr;
        }
        _count = 0;
        _open = -1;
        update();
        return;
}

GuiSize MenuBar::sizeHint() const {
        uint32_t w = 0;
        for (size_t i = 0; i < _count; ++i) w += (_titles[i]->textWidth() + 2u * MenuBarLayout::Pad + 3u) & ~3u;
        return GuiSize{static_cast<uint16_t>(w), Height};
}

void MenuBar::resized() {
        uint16_t widths[MaxMenus];
        Rect     places[MaxMenus];
        for (size_t i = 0; i < _count; ++i) widths[i] = _titles[i]->textWidth();
        const Rect &r = geometry();
        MenuBarLayout::place(Span<const uint16_t>(widths, _count), r.x, r.y, r.height, Span<Rect>(places, _count));
        for (size_t i = 0; i < _count; ++i) {
                // Past the bar's right edge: not shown.
                const bool fits = places[i].right() <= r.right();
                _titles[i]->setVisible(fits);
                if (fits) _titles[i]->setGeometry(places[i]);
        }
        update();
        return;
}

void MenuBar::paint(Painter &painter) {
        (void)painter.fill(geometry(), painter.palette().color(Role::Face));
        return;
}

bool MenuBar::open(size_t index, bool keyboard) {
        GuiWindow *w = window();
        if (index >= _count || w == nullptr || !_titles[index]->isVisible() || !_titles[index]->isEnabled())
                return false;
        if (_open >= 0) {
                // Another's first: its dismissal is a swap, not the bar's end.
                _opening = true;
                _menus[_open]->dismiss();
                _opening = false;
                _titles[_open]->update();
        }
        // Its callbacks this bar's (another bar may share it), its items filled.
        _menus[index]->setDismissed(dismissed, this);
        _menus[index]->setMoved(moved, this);
        if (_aboutToShow != nullptr) _aboutToShow(_aboutToShowArg, *this, index, *_menus[index]);
        const Rect  t = _titles[index]->geometry();
        const Rect  s = w->screenRect();
        const Error err = _menus[index]->popup(Rect{uint16_t(s.x + t.x), uint16_t(s.y + t.y), t.width, t.height});
        if (err.isError()) {
                _open = -1;
                return false;
        }
        _open = int(index);
        if (keyboard) _menus[index]->highlightFirst();
        _titles[index]->update();
        return true;
}

void MenuBar::hovered(size_t index) {
        if (_open >= 0 && size_t(_open) != index) (void)open(index, false);
        return;
}

int MenuBar::indexOf(const Menu &menu) const {
        for (size_t i = 0; i < _count; ++i)
                if (_menus[i] == &menu) return int(i);
        return -1;
}

void MenuBar::dismissed(void *arg, Menu &menu) {
        auto *self = static_cast<MenuBar *>(arg);
        if (self->_opening || self->indexOf(menu) != self->_open || self->_open < 0) return;
        self->_titles[self->_open]->update();
        self->_open = -1;
        return;
}

void MenuBar::moved(void *arg, Menu &menu, int step) {
        auto     *self = static_cast<MenuBar *>(arg);
        const int at = self->indexOf(menu);
        if (at < 0) return;
        // Round the bar to the next title shown.
        size_t next = size_t(at);
        for (size_t tries = 0; tries < self->_count; ++tries) {
                next = MenuBarLayout::neighbour(next, step, self->_count);
                if (self->_titles[next]->isVisible() && self->_titles[next]->isEnabled()) break;
        }
        if (next != size_t(at)) (void)self->open(next, true);
        return;
}

BMDS_NAMESPACE_END
