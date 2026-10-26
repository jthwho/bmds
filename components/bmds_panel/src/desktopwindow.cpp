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

#include <bmds/memory.h>
#include <bmds/region.h>

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
        _view->setDragAsked(viewDragAsked, this);
        _view->setDropHandlers(viewDropAsked, viewDropped, this);
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

void DesktopWindow::setItemIcon(size_t item, const Icon *icon, bool dimmed) {
        if (item >= _icons.size()) return;
        OwnedIcon &o = _icons[item];
        Memory::free(o.pixels);
        o = OwnedIcon();
        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                const size_t n = size_t(icon->width) * icon->height;
                o.pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                if (o.pixels != nullptr) {
                        for (size_t i = 0; i < n; ++i)
                                o.pixels[i] = dimmed ? halfAlpha(icon->pixels[i]) : icon->pixels[i];
                        o.icon = Icon{icon->width, icon->height, o.pixels};
                        o.dimmed = dimmed;
                }
        }
        _model.setIcons(item, nullptr, o.pixels != nullptr ? &o.icon : nullptr);
        if (_view != nullptr) _view->update();
        return;
}

Error DesktopWindow::setPattern(ByteSpan file, StringView colors, unsigned scale) {
        _desktopColor = application().windowManager().palette().color(Role::Desktop);
        const Error err = _pattern.load(file, colors, scale, _desktopColor);
        if (_view != nullptr) _view->setBackdrop(_pattern.isValid() ? &_pattern : nullptr);
        update();
        return err;
}

void DesktopWindow::clearPattern() {
        _pattern.reset();
        if (_view != nullptr) _view->setBackdrop(nullptr);
        update();
        return;
}

// Around the view (the panel's strip when its window is above): the wallpaper, or the color (the
// view paints its own; filling under it too drew the desktop twice). Painted before the view, so
// a new desktop color is blended into the pattern first.
void DesktopWindow::paint(Painter &painter) {
        const uint32_t desktop = painter.palette().color(Role::Desktop);
        if (_pattern.isValid() && desktop != _desktopColor) {
                _desktopColor = desktop;
                _pattern.setBackground(desktop);
                if (!_pattern.isValid() && _view != nullptr) _view->setBackdrop(nullptr); // no memory
        }
        Rect   parts[4];
        size_t n = 1;
        parts[0] = geometry();
        if (_view != nullptr) n = subtract(geometry(), _view->geometry(), parts);
        if (!_pattern.isValid()) {
                (void)painter.fill(Span<const Rect>(parts, n), desktop);
                return;
        }
        for (size_t i = 0; i < n; ++i) (void)_pattern.paint(painter, parts[i]);
        return;
}

void DesktopWindow::activeChanged() {
        if (_view != nullptr) _view->update(); // the selection filled while active, outlined while not
        return;
}

void DesktopWindow::drawReference(uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                                  const WindowPalette &palette) {
        const uint32_t desktop = palette.color(Role::Desktop);
        if (_pattern.isValid()) {
                _pattern.make(0, 0, width, height, pixels, stride);
        } else {
                for (uint32_t y = 0; y < height; ++y)
                        for (uint32_t x = 0; x < width; ++x) {
                                uint8_t *p = pixels + y * stride + x * 3;
                                p[0] = static_cast<uint8_t>(desktop);
                                p[1] = static_cast<uint8_t>(desktop >> 8);
                                p[2] = static_cast<uint8_t>(desktop >> 16);
                        }
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

// ---- Drag and drop (devplan/clipboard.md §5e)

bool DesktopWindow::viewDragAsked(void *arg, ItemView &, GuiDrag &drag) {
        auto *self = static_cast<DesktopWindow *>(arg);
        return self->_dragAsked != nullptr && self->_dragAsked(self->_dragArg, drag);
}

// Its own icons over the desktop itself or over one of them, without Ctrl: moved on the grid.
bool DesktopWindow::ownOnGrid(int row, const GuiDropEvent &event) const {
        if (!event.isOwn() || event.source() != _view || (event.modifiers() & KeyEvent::Ctrl) != 0) return false;
        return row < 0 || _view->selection().isSelected(size_t(row));
}

DropAction DesktopWindow::viewDropAsked(void *arg, ItemView &, int &row, const GuiDropEvent &event) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (self->ownOnGrid(row, event)) {
                row = -1;
                return DropAction::Move;
        }
        return self->_dropAsked != nullptr ? self->_dropAsked(self->_dropArg, row, event) : DropAction::None;
}

bool DesktopWindow::viewDropped(void *arg, ItemView &, int row, GuiDropEvent &event) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (row < 0 && self->ownOnGrid(row, event) && !event.isAsk()) {
                int x = 0, y = 0;
                self->_view->dragOrigin(x, y);
                (void)self->_view->moveSelected(event.x() - x, event.y() - y);
                event.finish(DropAction::Moved);
                return true;
        }
        return self->_dropped != nullptr && self->_dropped(self->_dropArg, row, event);
}

void DesktopWindow::moved(void *arg, IconView &) {
        auto *self = static_cast<DesktopWindow *>(arg);
        if (self->_moved != nullptr) self->_moved(self->_movedArg);
        return;
}

BMDS_NAMESPACE_END
