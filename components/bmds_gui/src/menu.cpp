/**
 * @file      menu.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Popup menus (devplan/gui.md §4b).
 */

#include <cstring>
#include <bmds/bitmapfont.h>
#include <bmds/menu.h>
#include <bmds/systemfonts.h>

BMDS_NAMESPACE_BEGIN

namespace {

        GuiWindow::Config menuConfig() {
                GuiWindow::Config c;
                c.title = "Menu";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Popup;
                c.focusable = false;
                c.kind = "menu";
                return c;
        }

        void copyText(char *out, size_t size, StringView text) {
                size_t n = text.size() < size - 1 ? text.size() : size - 1;
                if (n < text.size())
                        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xc0u) == 0x80u) --n;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

} // namespace

Menu::Menu(GuiApplication &app) : GuiWindow(app, menuConfig()) {}

Menu::~Menu() {
        closeSubmenu();
        if (_parent != nullptr && _parent->_open == this) _parent->_open = nullptr;
        _parent = nullptr;
}

Error Menu::add(StringView text, int number, uint8_t flags, StringView shortcut, Menu *submenu, const Icon *icon) {
        if (_count == MaxItems) return Error::Full;
        Item &item = _items[_count++];
        item = Item();
        const Mnemonic::Stripped m = Mnemonic::strip(text, Span<char>(item.text, sizeof(item.text)));
        item.underline = m.at;
        item.key = m.key;
        copyText(item.shortcut, sizeof(item.shortcut), shortcut);
        item.number = number;
        item.flags = flags;
        item.submenu = submenu;
        item.icon = icon;
        return Error();
}

Error Menu::addItem(StringView text, int item, uint8_t flags, StringView shortcut, const Icon *icon) {
        return add(text, item, static_cast<uint8_t>(flags & (MenuLayout::Disabled | MenuLayout::Checked)), shortcut,
                   nullptr, icon);
}

Error Menu::addSeparator() {
        return add(StringView(), 0, MenuLayout::Separator, StringView(), nullptr, nullptr);
}

Error Menu::addSubmenu(StringView text, Menu &submenu, const Icon *icon) {
        return add(text, 0, MenuLayout::Submenu, StringView(), &submenu, icon);
}

void Menu::clear() {
        closeSubmenu();
        close();
        _count = 0;
        _highlight = -1;
        return;
}

Error Menu::popup(const Rect &anchor, bool submenu) {
        if (_count == 0) return Error::Invalid;
        closeSubmenu();
        close();
        for (size_t i = 0; i < _count; ++i)
                _views[i] = MenuLayout::Item{StringView(_items[i].text), StringView(_items[i].shortcut),
                                             _items[i].flags, _items[i].icon, _items[i].underline};
        // Menus are in the ui role's font, as the window manager's are.
        FontName ui;
        ui.role = FontName::Role::Ui;
        auto [font, ferr] = SystemFonts::library().open(ui);
        if (ferr.isError()) return ferr;
        const Error err = _layout.layout(Span<const MenuLayout::Item>(_views, _count), font);
        if (err.isError()) return err;
        const Rect area = application().windowManager().workArea();
        _highlight = -1;
        return show(MenuLayout::place(_layout.width(), _layout.height(), anchor, area, submenu));
}

Menu &Menu::root() {
        Menu *m = this;
        while (m->_parent != nullptr) m = m->_parent;
        return *m;
}

void Menu::closeTree() {
        const bool shown = isShown();
        closeSubmenu();
        close();
        _highlight = -1;
        if (shown && _parent == nullptr && _dismissed != nullptr) _dismissed(_dismissedArg, *this);
        return;
}

void Menu::dismiss() {
        Menu &r = root();
        r.closeTree();
        return;
}

void Menu::closeSubmenu() {
        if (_open == nullptr) return;
        Menu *sub = _open;
        _open = nullptr;
        sub->closeTree();
        sub->_parent = nullptr;
        return;
}

void Menu::closedByManager() {
        // Closed by a press outside the popups: the others go too.
        if (_parent == nullptr && _dismissed != nullptr) _dismissed(_dismissedArg, *this);
        if (_parent != nullptr && _parent->_open == this) _parent->_open = nullptr;
        _parent = nullptr;
        _open = nullptr;
        _highlight = -1;
        GuiWindow::closedByManager();
        return;
}

void Menu::highlight(int item) {
        if (item == _highlight) return;
        for (const int i : {_highlight, item})
                if (i >= 0) invalidate(_layout.itemRect(size_t(i)));
        _highlight = item;
        return;
}

void Menu::choose(int item) {
        if (item < 0 || size_t(item) >= _count || !_layout.isSelectable(size_t(item))) return;
        if (_items[item].submenu != nullptr) {
                openSubmenu(item, true);
                return;
        }
        Menu           &r = root();
        const int       number = _items[item].number;
        const Triggered fn = r._triggered;
        void           *arg = r._triggeredArg;
        r.closeTree();
        if (fn != nullptr) fn(arg, number);
        return;
}

void Menu::openSubmenu(int item, bool select) {
        Menu *sub = _items[item].submenu;
        if (sub == nullptr) return;
        if (_open != sub) {
                closeSubmenu();
                const Rect r = _layout.itemRect(size_t(item));
                const Rect s = screenRect();
                const Rect anchor{uint16_t(s.x + r.x), uint16_t(s.y + r.y), r.width, r.height};
                sub->_parent = this;
                if (sub->popup(anchor, true).isError()) {
                        sub->_parent = nullptr;
                        return;
                }
                _open = sub;
        }
        if (select) sub->highlight(sub->_layout.next(-1, 1));
        return;
}

void Menu::paint(Painter &painter) {
        drawMenu(painter, _layout, Span<const MenuLayout::Item>(_views, _count), _highlight);
        return;
}

void Menu::mousePressed(const WindowManager::MouseEvent &event) {
        mouseMoved(event);
        return;
}

void Menu::mouseMoved(const WindowManager::MouseEvent &event) {
        const int item = _layout.itemAt(event.x, event.y);
        if (item < 0 || !_layout.isSelectable(size_t(item))) {
                if (_open == nullptr) highlight(-1);
                return;
        }
        highlight(item);
        if (_items[item].submenu != nullptr) {
                openSubmenu(item, false);
        } else {
                closeSubmenu();
        }
        return;
}

void Menu::mouseReleased(const WindowManager::MouseEvent &event) {
        const int item = _layout.itemAt(event.x, event.y);
        if (item >= 0 && _items[item].submenu == nullptr) choose(item);
        return;
}

void Menu::mouseLeft() {
        if (_open == nullptr) highlight(-1);
        return;
}

bool Menu::keyPressed(const KeyEvent &event) {
        switch (event.usage) {
                case HidKey::Down:
                case HidKey::Up: highlight(_layout.next(_highlight, event.usage == HidKey::Down ? 1 : -1)); return true;
                case HidKey::Right:
                        if (_highlight >= 0 && _items[_highlight].submenu != nullptr) {
                                openSubmenu(_highlight, true);
                        } else if (root()._moved != nullptr) {
                                Menu &r = root();
                                r._moved(r._movedArg, r, 1);
                        }
                        return true;
                case HidKey::Left:
                        if (_parent != nullptr) {
                                _parent->closeSubmenu();
                        } else if (_moved != nullptr) {
                                _moved(_movedArg, *this, -1);
                        }
                        return true;
                case HidKey::Enter:
                case HidKey::KeypadEnter:
                        if (_highlight >= 0) choose(_highlight);
                        return true;
                case HidKey::Escape:
                        if (_parent != nullptr) {
                                _parent->closeSubmenu();
                        } else {
                                dismiss();
                        }
                        return true;
                default: {
                        const uint32_t key = Mnemonic::keyOf(event);
                        if (key == 0) return false;
                        chooseMnemonic(key);
                        return true;
                }
        }
}

// A key typed in it: the one item it marks chosen (a submenu opened); with several, the next
// of them highlighted.
void Menu::chooseMnemonic(uint32_t key) {
        int    first = -1, after = -1;
        size_t count = 0;
        for (size_t i = 0; i < _count; ++i) {
                if (_items[i].key != key || !_layout.isSelectable(i)) continue;
                ++count;
                if (first < 0) first = int(i);
                if (after < 0 && int(i) > _highlight) after = int(i);
        }
        if (count == 1) {
                highlight(first);
                if (_items[first].submenu != nullptr) {
                        openSubmenu(first, true);
                } else {
                        choose(first);
                }
        } else if (count > 1) {
                highlight(after >= 0 ? after : first);
        }
        return;
}

BMDS_NAMESPACE_END
