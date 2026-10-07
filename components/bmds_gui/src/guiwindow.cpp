/**
 * @file      guiwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A toolkit window on the window manager (devplan/gui.md §4b).
 */

#include <cstring>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;
        using Type = GuiApplication::Event::Type;

} // namespace

GuiWindow::GuiWindow(GuiApplication &app, const Config &config) : Widget(nullptr), _app(app), _config(config) {
        setWindow(this);
        if (config.title != nullptr) {
                std::strncpy(_title, config.title, WindowManager::MaxTitle);
                _title[WindowManager::MaxTitle] = '\0';
        }
        if (config.kind != nullptr) {
                std::strncpy(_kind, config.kind, WindowManager::MaxKind);
                _kind[WindowManager::MaxKind] = '\0';
        }
        _config.title = _title;
        _config.kind = config.kind != nullptr ? _kind : nullptr;
        auto [slot, err] = app.addWindow(*this);
        _slot = err.isOk() ? slot : GuiApplication::MaxWindows; // no events without one
        _generation = app.generation(_slot);
}

GuiWindow::~GuiWindow() {
        close();
        _app.removeWindow(*this);
        _grab = _hover = _focus = _central = nullptr;
}

Error GuiWindow::show() {
        return show(_config.rect);
}

Error GuiWindow::show(const Rect &rect) {
        if (isShown()) return Error();
        if (_slot >= GuiApplication::MaxWindows) return Error::Full;
        WindowManager::WindowConfig c;
        c.rect = rect;
        c.title = _title;
        c.redraw = redraw;
        c.closed = closedEntry;
        c.key = keyEntry;
        c.resized = resizedEntry;
        c.wheel = wheelEntry;
        c.mouse = mouseEntry;
        c.arg = this;
        c.decorations = _config.decorations;
        c.level = _config.level;
        c.reserve = _config.reserve;
        c.focusable = _config.focusable;
        c.kind = _config.kind;
        c.pointer = _config.pointer;
        c.client = _app.client();
        c.reference = _config.reference ? referenceEntry : nullptr;
        WindowManager &wm = _app.windowManager();
        // Its events from now on; anything queued for an earlier opening dropped.
        _app.removeWindow(*this);
        auto [slot, serr] = _app.addWindow(*this);
        if (serr.isError()) return serr;
        _slot = slot;
        _generation = _app.generation(slot);
        auto [id, err] = wm.open(c);
        if (err.isError()) return err;
        _id.store(id, std::memory_order_release);
        if (_icon != nullptr) (void)wm.setIcon(id, _icon);
        auto [client, cerr] = wm.clientRect(id);
        _screen = cerr.isOk() ? client : Rect();
        _dirty.clear();
        setGeometry(Rect{0, 0, _screen.width, _screen.height});
        return Error();
}

void GuiWindow::close() {
        const uint8_t id = _id.exchange(0, std::memory_order_acq_rel);
        if (id != 0) (void)_app.windowManager().close(id);
        _screen = Rect();
        _grab = _hover = nullptr;
        _dirty.clear();
        return;
}

void GuiWindow::setIcon(const Icon *icon) {
        _icon = icon;
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().setIcon(id, icon);
        return;
}

void GuiWindow::setCentral(Widget *widget) {
        _central = widget;
        if (widget != nullptr) widget->setGeometry(Rect{0, 0, geometry().width, geometry().height});
        return;
}

void GuiWindow::resized() {
        if (_central != nullptr) _central->setGeometry(Rect{0, 0, geometry().width, geometry().height});
        return;
}

void GuiWindow::invalidate(const Rect &rect) {
        if (isShown()) _dirty.add(rect);
        return;
}

void GuiWindow::flush() {
        const uint8_t id = this->id();
        if (id == 0 || _dirty.isEmpty()) return;
        WindowManager &wm = _app.windowManager();
        for (const Rect &r : _dirty.rects()) (void)wm.invalidate(id, r);
        _dirty.clear();
        return;
}

void GuiWindow::forget(Widget *widget) {
        // Its children are being destroyed too: anything inside it.
        for (Widget **w : {&_grab, &_hover, &_focus, &_central}) {
                for (Widget *p = *w; p != nullptr; p = p->parent()) {
                        if (p == widget) {
                                *w = nullptr;
                                break;
                        }
                }
        }
        return;
}

void GuiWindow::paint(Painter &painter) {
        if (_central == nullptr) (void)painter.fill(geometry(), painter.palette().color(Role::Face));
        return;
}

void GuiWindow::paintTree(Widget &widget, Painter &painter) {
        if (!widget.isVisible() || !painter.isVisible(widget.geometry())) return;
        widget.paint(painter);
        for (Widget *c = widget.firstChild(); c != nullptr; c = c->next()) paintTree(*c, painter);
        return;
}

// On the window manager's task: drawn now if the application isn't busy,
// else at the next round (the window manager's lock is held: no waiting).
void GuiWindow::redraw(void *arg, Painter &painter) {
        auto  *self = static_cast<GuiWindow *>(arg);
        Mutex &lock = self->_app.lock();
        if (lock.tryLock().isError()) {
                const uint8_t id = self->id();
                if (id != 0) (void)self->_app.windowManager().update(id);
                return;
        }
        self->paintTree(*self, painter);
        lock.unlock();
        return;
}

void GuiWindow::queue(GuiApplication::Event event) {
        event.slot = _slot;
        event.generation = _generation;
        _app.queue(event);
        return;
}

void GuiWindow::closedEntry(void *arg) {
        GuiApplication::Event e;
        e.type = Type::Closed;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::keyEntry(void *arg, const KeyEvent &event) {
        GuiApplication::Event e;
        e.type = Type::Key;
        e.key = event;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::resizedEntry(void *arg, uint16_t width, uint16_t height) {
        GuiApplication::Event e;
        e.type = Type::Resized;
        e.width = width;
        e.height = height;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::wheelEntry(void *arg, int notches) {
        GuiApplication::Event e;
        e.type = Type::Wheel;
        e.wheel = static_cast<int16_t>(notches);
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::mouseEntry(void *arg, const WindowManager::MouseEvent &event) {
        GuiApplication::Event e;
        e.type = Type::Mouse;
        e.mouse = event;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::referenceEntry(void *arg, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height,
                               const WindowPalette &palette) {
        static_cast<GuiWindow *>(arg)->drawReference(pixels, stride, width, height, palette);
        return;
}

void GuiWindow::handle(const GuiApplication::Event &event) {
        switch (event.type) {
                case Type::Mouse: mouse(event.mouse); break;
                case Type::Key: {
                        if (event.key.action == KeyEvent::Action::Release) break;
                        bool used = _focus != nullptr && _focus->isEnabled() && _focus->keyPressed(event.key);
                        if (!used && _focus != this) used = keyPressed(event.key);
                        if (!used) (void)keyUnused(event.key);
                        break;
                }
                case Type::Resized: {
                        // Where it is now, too (a maximize moves it).
                        const uint8_t id = this->id();
                        if (id != 0) {
                                auto [client, err] = _app.windowManager().clientRect(id);
                                if (err.isOk()) _screen = client;
                        }
                        setGeometry(Rect{0, 0, event.width, event.height});
                        break;
                }
                case Type::Closed:
                        if (_id.exchange(0, std::memory_order_acq_rel) != 0) {
                                _screen = Rect();
                                _grab = _hover = nullptr;
                                _dirty.clear();
                                closedByManager();
                        }
                        break;
                default: break;
        }
        return;
}

void GuiWindow::closedByManager() {
        if (_closed != nullptr) _closed(_closedArg, *this);
        return;
}

void GuiWindow::mouse(const WindowManager::MouseEvent &event) {
        using MType = WindowManager::MouseEvent::Type;
        // The window may have moved since it was opened: its place, for screen coordinates.
        if (event.type == MType::Press) {
                auto [client, err] = _app.windowManager().clientRect(id());
                if (err.isOk()) _screen = client;
        }
        switch (event.type) {
                case MType::Press: {
                        Widget *w = widgetAt(event.x, event.y);
                        if (_grab == nullptr) _grab = w;
                        if (_hover != w && _hover != nullptr) _hover->mouseLeft();
                        _hover = w;
                        if (w->isEnabled()) w->mousePressed(event);
                        break;
                }
                case MType::Release: {
                        Widget *w = _grab != nullptr ? _grab : widgetAt(event.x, event.y);
                        if (event.buttons == 0) _grab = nullptr;
                        if (w->isEnabled()) w->mouseReleased(event);
                        break;
                }
                case MType::Motion: {
                        if (_grab != nullptr) {
                                _grab->mouseMoved(event);
                                break;
                        }
                        Widget *w = widgetAt(event.x, event.y);
                        if (_hover != w && _hover != nullptr) _hover->mouseLeft();
                        _hover = w;
                        if (w->isEnabled()) w->mouseMoved(event);
                        break;
                }
                case MType::Leave:
                        // Left, or a popup took the press held: no release will come.
                        if (_hover != nullptr) _hover->mouseLeft();
                        if (_grab != nullptr && _grab != _hover) _grab->mouseLeft();
                        _hover = nullptr;
                        _grab = nullptr;
                        break;
        }
        return;
}

BMDS_NAMESPACE_END
