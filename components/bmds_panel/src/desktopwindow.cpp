/**
 * @file      desktopwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's desktop window: an icon view on the desktop's grid (devplan/gui.md §4m,
 * devplan/filebrowser.md §4g).
 */

#include <cstring>
#include <bmds/memory.h>

#include "desktopwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

} // namespace

DesktopWindow::DesktopWindow(GuiApplication &app, const Config &config) : GuiWindow(app, config) {
        _view = Widget::create<IconView>(this);
        if (_view == nullptr) return;
        _view->setArrangement(IconView::Arrangement::Grid);
        _view->setColors(Role::Desktop, Role::DesktopText);
        _view->setModel(&_model);
        _view->setActivated(activated, this);
        _view->setMenuAsked(menuAsked, this);
        _view->setRenameAsked(renameAsked, this);
        _view->setKeyTyped(keyTyped, this);
        _view->setMoved(moved, this);
        setFocusWidget(_view);
}

DesktopWindow::~DesktopWindow() {
        _view = nullptr; // destroyed with the window's children
        freeIcons();
}

void DesktopWindow::freeIcons() {
        for (OwnedIcon &o : _icons) Memory::free(o.pixels);
        _icons.clear();
        return;
}

int DesktopWindow::selected() const {
        if (_view == nullptr) return -1;
        const int current = _view->current();
        if (current >= 0 && _view->selection().isSelected(size_t(current))) return current;
        return _view->selection().nextSelected(0);
}

void DesktopWindow::setSelected(int item) {
        if (_view != nullptr) _view->selectOnly(item >= 0 && size_t(item) < _model.count() ? item : -1);
        return;
}

Error DesktopWindow::setItems(Span<const StringView> names, Span<const IconGrid::Cell> wanted, const Rect &area) {
        _model.clear();
        freeIcons();
        Error err = _icons.reserve(names.size());
        for (size_t i = 0; i < names.size() && err.isOk(); ++i) {
                err = _model.add(names[i], nullptr, nullptr);
                if (err.isOk()) (void)_icons.append(OwnedIcon());
        }
        if (_view == nullptr) return Error::NoMem;
        _view->setGeometry(area);
        _view->modelReset();
        const Error placed = _view->setCells(wanted.size() == _model.count() ? wanted : Span<const IconGrid::Cell>());
        update();
        return err.isError() ? err : placed;
}

void DesktopWindow::setItemIcon(size_t item, const Icon *icon) {
        if (item >= _icons.size()) return;
        OwnedIcon &o = _icons[item];
        Memory::free(o.pixels);
        o = OwnedIcon();
        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                const size_t n = size_t(icon->width) * icon->height;
                o.pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                if (o.pixels != nullptr) {
                        std::memcpy(o.pixels, icon->pixels, n * sizeof(uint32_t));
                        o.icon = Icon{icon->width, icon->height, o.pixels};
                }
        }
        _model.setIcons(item, nullptr, o.pixels != nullptr ? &o.icon : nullptr);
        if (_view != nullptr) _view->update();
        return;
}

// Around the view (the panel's strip when its window is above): the desktop's colour.
void DesktopWindow::paint(Painter &painter) {
        (void)painter.fill(geometry(), painter.palette().color(Role::Desktop));
        return;
}

void DesktopWindow::activeChanged() {
        if (_view != nullptr) _view->update(); // the selection filled while active, outlined while not
        return;
}

void DesktopWindow::drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                  const WindowPalette &palette) {
        const uint32_t desktop = palette.color(Role::Desktop);
        for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                        uint8_t *p = pixels + y * stride + x * 3;
                        p[0] = static_cast<uint8_t>(desktop);
                        p[1] = static_cast<uint8_t>(desktop >> 8);
                        p[2] = static_cast<uint8_t>(desktop >> 16);
                }
        if (_view != nullptr) _view->drawReference(pixels, stride, width, height, palette);
        return;
}

void DesktopWindow::activated(void *arg, ItemView &, size_t row) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (self->_opened != nullptr) self->_opened(self->_openedArg, row);
        return;
}

void DesktopWindow::menuAsked(void *arg, ItemView &, int row, uint16_t x, uint16_t y) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (row >= 0 && self->_itemMenuAsked != nullptr) {
                self->_itemMenuAsked(self->_itemMenuArg, size_t(row), x, y);
        } else if (self->_menuAsked != nullptr) {
                self->_menuAsked(self->_menuArg, x, y);
        }
        return;
}

void DesktopWindow::renameAsked(void *arg, ItemView &, size_t row) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (self->_renameAsked != nullptr) self->_renameAsked(self->_renameArg, row);
        return;
}

bool DesktopWindow::keyTyped(void *arg, ItemView &, const KeyEvent &event) {
        auto *self = static_cast<DesktopWindow *>(arg);
        return self->_keyTyped != nullptr && self->_keyTyped(self->_keyArg, event, self->selected());
}

void DesktopWindow::moved(void *arg, IconView &) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (self->_moved != nullptr) self->_moved(self->_movedArg);
        return;
}

BMDS_NAMESPACE_END
