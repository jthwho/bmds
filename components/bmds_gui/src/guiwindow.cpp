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
#include <bmds/menubar.h>

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
        _default = nullptr;
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
        c.closedAt = closedEntry;
        c.key = keyEntry;
        c.pad = padEntry;
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
        _pointer = _config.pointer;
        c.client = _app.client();
        c.reference = _config.reference ? referenceEntry : nullptr;
        c.activated = activatedEntry;
        c.focusOnPress = _config.focusOnPress;
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
        if (_largeIcon != nullptr) (void)wm.setLargeIcon(id, _largeIcon);
        auto [client, cerr] = wm.clientRect(id);
        _screen = cerr.isOk() ? client : Rect();
        _dirty.clear();
        setGeometry(Rect{0, 0, _screen.width, _screen.height});
        return Error();
}

void GuiWindow::close() {
        const uint8_t id = _id.exchange(0, std::memory_order_acq_rel);
        if (id != 0) {
                auto [outer, err] = _app.windowManager().rect(id);
                if (err.isOk()) _lastRect = outer;
                (void)_app.windowManager().close(id);
        }
        _screen = Rect();
        _grab = _hover = nullptr;
        _active = false;
        _dirty.clear();
        return;
}

Error GuiWindow::move(uint16_t x, uint16_t y) {
        const uint8_t id = this->id();
        if (id == 0) return Error::InvalidState;
        WindowManager &wm = _app.windowManager();
        const Error    err = wm.move(id, x, y);
        if (err.isError()) return err;
        auto [client, cerr] = wm.clientRect(id);
        if (cerr.isOk()) _screen = client;
        return Error::Ok;
}

void GuiWindow::setFocusWidget(Widget *widget) {
        if (widget == _focus) return;
        Widget *old = _focus;
        if (old != nullptr) old->update();
        _focus = widget;
        if (widget != nullptr) widget->update();
        // The old one commits (which may move the focus again, or destroy the new one: then
        // it is told nothing more).
        if (old != nullptr) old->focusOut();
        if (widget == nullptr || _focus != widget) return;
        widget->focusIn();
        for (Widget *p = widget->parent(); p != nullptr && _focus == widget; p = p->parent()) p->childFocused(*widget);
        return;
}

void GuiWindow::setDefaultButton(Button *button) {
        if (button == _default) return;
        if (_default != nullptr) _default->update();
        _default = button;
        if (_default != nullptr) _default->update();
        return;
}

void GuiWindow::activate() {
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().raise(id);
        return;
}

namespace {

        // @p fn(widget, its place in tree order) for @p root and every visible, enabled widget under it.
        template <typename Fn> void walk(Widget *root, Fn &fn, size_t &at) {
                if (!root->isVisible() || !root->isEnabled()) return;
                fn(root, at++);
                for (Widget *c = root->firstChild(); c != nullptr; c = c->next()) walk(c, fn, at);
                return;
        }

        constexpr size_t MaxFocusable = 64;

} // namespace

bool GuiWindow::focusNext(bool forward) {
        Widget *list[MaxFocusable];
        size_t  n = 0, at = 0;
        int     current = -1;
        auto    collect = [&](Widget *w, size_t) {
                if (w == this || !w->isFocusable() || n == MaxFocusable) return;
                if (w == _focus) current = int(n);
                list[n++] = w;
        };
        walk(this, collect, at);
        const int next = forward ? current + 1 : (current < 0 ? int(n) - 1 : current - 1);
        if (n == 0 || next < 0 || next >= int(n)) {
                if (_tabOut != nullptr) {
                        _tabOut(_tabOutArg, *this, forward);
                        return true;
                }
                if (n == 0) return false;
                setFocusWidget(list[forward ? 0 : n - 1]); // around
                return true;
        }
        setFocusWidget(list[next]);
        return true;
}

bool GuiWindow::focusFirst(bool last) {
        Widget *found = nullptr;
        size_t  at = 0;
        auto    pick = [&](Widget *w, size_t) {
                if (w != this && w->isFocusable() && (last || found == nullptr)) found = w;
        };
        walk(this, pick, at);
        if (found == nullptr) return false;
        setFocusWidget(found);
        return true;
}

// Widgets with mnemonic @p key: one acts; several take the focus in turn, from the focus on.
bool GuiWindow::activateMnemonic(uint32_t key) {
        Widget *first = nullptr, *after = nullptr;
        size_t  count = 0, at = 0, focusAt = 0;
        bool    focusSeen = false;
        auto    match = [&](Widget *w, size_t place) {
                if (w == _focus) {
                        focusAt = place;
                        focusSeen = true;
                }
                if (w->mnemonic() != key) return;
                ++count;
                if (first == nullptr) first = w;
                if (after == nullptr && focusSeen && place > focusAt) after = w;
        };
        walk(this, match, at);
        if (count == 0) return false;
        if (count == 1) {
                first->mnemonicActivated();
        } else {
                setFocusWidget(after != nullptr ? after : first);
        }
        return true;
}

void GuiWindow::setIcon(const Icon *icon) {
        _icon = icon;
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().setIcon(id, icon);
        return;
}

void GuiWindow::setLargeIcon(const Icon *icon) {
        _largeIcon = icon;
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().setLargeIcon(id, icon);
        return;
}

void GuiWindow::setTitle(StringView title) {
        const size_t n = title.size() < WindowManager::MaxTitle ? title.size() : WindowManager::MaxTitle;
        std::memcpy(_title, title.data(), n);
        _title[n] = '\0';
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().setTitle(id, StringView(_title, n));
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

uint8_t GuiWindow::takeDirty(Region &out) {
        out = _dirty;
        _dirty.clear();
        return id();
}

void GuiWindow::forget(Widget *widget) {
        _app.forgetToolTip(widget);
        // Its children are being destroyed too: anything inside it.
        if (_default != nullptr) {
                for (Widget *p = _default; p != nullptr; p = p->parent()) {
                        if (p == widget) {
                                _default = nullptr;
                                break;
                        }
                }
        }
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
        for (Widget *c = widget.firstChild(); c != nullptr; c = c->next()) {
                Rect clip;
                if (!widget.childClip(*c, clip)) {
                        paintTree(*c, painter);
                        continue;
                }
                // Kept inside its clip (a scrolled view's rows): through a painter of its own.
                const Rect shown = c->geometry().intersected(clip);
                if (!c->isVisible() || shown.isEmpty()) continue;
                GuiStyle::drawClipped(painter, shown, [this, c](Painter &clipped) { paintTree(*c, clipped); });
        }
        return;
}

// On the window manager's task: drawn now if the application isn't busy,
// else at the next round (the window manager's lock is held: no waiting).
void GuiWindow::redraw(void *arg, Painter &painter) {
        auto  *self = static_cast<GuiWindow *>(arg);
        Mutex &lock = self->_app.lock();
        if (lock.tryLock().isError()) {
                painter.defer(); // its damage again next round, not the whole window
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

void GuiWindow::closedEntry(void *arg, const Rect &outer) {
        GuiApplication::Event e;
        e.type = Type::Closed;
        e.x = outer.x;
        e.y = outer.y;
        e.width = outer.width;
        e.height = outer.height;
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

void GuiWindow::padEntry(void *arg, const PadEvent &event) {
        GuiApplication::Event e;
        e.type = Type::Pad;
        e.pad = event;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

void GuiWindow::activatedEntry(void *arg, bool active) {
        GuiApplication::Event e;
        e.type = Type::Activated;
        e.width = active ? 1 : 0;
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
                case Type::Key: key(event.key); break;
                case Type::Pad: padEvent(event.pad); break;
                case Type::Wheel: wheel(event.wheel); break;
                case Type::Activated: {
                        const bool active = event.width != 0 && isShown();
                        if (active == _active) break;
                        _active = active;
                        if (_focus != nullptr) _focus->update();
                        activeChanged();
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
                                if (event.width != 0) _lastRect = Rect{event.x, event.y, event.width, event.height};
                                _screen = Rect();
                                _grab = _hover = nullptr;
                                _active = false;
                                _dirty.clear();
                                closedByManager();
                        }
                        break;
                default: break;
        }
        return;
}

// A key, in the order the class's description gives.
void GuiWindow::key(const KeyEvent &event) {
        if (keyFirst(event)) return;
        if (event.action == KeyEvent::Action::Release) return;
        _app.hideToolTip();
        const uint32_t key = Mnemonic::keyOf(event);
        if (key != 0 && event.hasAlt() && activateMnemonic(key)) return;
        bool used = false;
        if (_focus != nullptr && _focus != this)
                for (Widget *w = _focus->parent(); w != nullptr && w != this && !used; w = w->parent())
                        used = w->isEnabled() && w->keyBeforeChild(event);
        if (!used) used = _focus != nullptr && _focus->isEnabled() && _focus->keyPressed(event);
        if (!used && _focus != this) used = keyPressed(event);
        if (!used && event.usage == HidKey::Tab && !event.hasCtrl() && !event.hasAlt())
                used = focusNext(!event.hasShift());
        if (!used && (event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter) && _default != nullptr &&
            _default->isVisible() && _default->isEnabled()) {
                _default->click();
                used = true;
        }
        if (!used && key != 0 && !event.hasAlt()) used = activateMnemonic(key);
        if (!used && event.usage == HidKey::F1 + 9 && event.modifiers == 0 && _menuBar != nullptr &&
            _menuBar->isVisible())
                used = _menuBar->openFirst();
        if (!used) (void)keyUnused(event);
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
                        _app.hideToolTip();
                        Widget *w = widgetAt(event.x, event.y);
                        if (_grab == nullptr) _grab = w;
                        if (w != this && w->isFocusable() && w->isEnabled()) setFocusWidget(w);
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
                        if (!_pointerInside) {
                                _pointerInside = true;
                                pointerEntered();
                        }
                        if (_grab != nullptr) {
                                pointerOver(_grab, event.x, event.y);
                                _grab->mouseMoved(event);
                                break;
                        }
                        Widget *w = widgetAt(event.x, event.y);
                        if (_hover != w) {
                                if (_hover != nullptr) _hover->mouseLeft();
                                // Dragged by the window manager since: where it is, for the tooltip.
                                auto [client, err] = _app.windowManager().clientRect(id());
                                if (err.isOk()) _screen = client;
                        }
                        _hover = w;
                        pointerOver(w, event.x, event.y);
                        if (w->isEnabled()) w->mouseMoved(event);
                        // After the widget saw it: a tooltip by the part under the pointer (a tray's
                        // icon) is known from the first move onto it.
                        _app.hoverToolTip(w, _screen.x + event.x, _screen.y + event.y);
                        break;
                }
                case MType::Leave:
                        _app.hoverToolTip(nullptr, 0, 0);
                        if (_pointerInside) {
                                _pointerInside = false;
                                pointerLeft();
                        }
                        // Left, or a popup took the press held: no release will come.
                        if (_hover != nullptr) _hover->mouseLeft();
                        if (_grab != nullptr && _grab != _hover) _grab->mouseLeft();
                        _hover = nullptr;
                        _grab = nullptr;
                        break;
        }
        return;
}

// The wheel: to the widget under the pointer, then its parents, until one uses it.
void GuiWindow::wheel(int notches) {
        for (Widget *w = _hover; w != nullptr; w = w->parent())
                if (w->isVisible() && w->isEnabled() && w->wheelTurned(notches)) return;
        return;
}

WindowManager::Pointer GuiWindow::pointerAt(int x, int y) const {
        (void)x;
        (void)y;
        return _config.pointer;
}

void GuiWindow::pointerOver(Widget *widget, int x, int y) {
        const WindowManager::Pointer shape = widget->pointerAt(x, y);
        if (shape == _pointer || !isShown()) return;
        _pointer = shape;
        (void)_app.windowManager().setPointer(id(), shape);
        return;
}

BMDS_NAMESPACE_END
