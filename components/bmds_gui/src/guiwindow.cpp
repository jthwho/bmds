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
        c.drop = dropEntry;
        c.arg = this;
        c.decorations = _config.decorations;
        c.level = _config.level;
        c.session = _app.session();
        c.reserve = _config.reserve;
        c.focusable = _config.focusable;
        c.kind = _config.kind;
        c.pointer = _config.pointer;
        _pointer = _config.pointer;
        c.client = _app.client();
        c.reference = _config.reference ? referenceEntry : nullptr;
        c.activated = activatedEntry;
        c.closeRequested = closeRequestedEntry;
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
        if (_minWidth != 0 || _minHeight != 0) (void)wm.setMinimumSize(id, _minWidth, _minHeight);
        if (_forcedPointer != WindowManager::Pointer::Arrow) {
                _pointer = _forcedPointer;
                (void)wm.setPointer(id, _forcedPointer);
        }
        auto [client, cerr] = wm.clientRect(id);
        _screen = cerr.isOk() ? client : Rect();
        _dirty.clear();
        setGeometry(Rect{0, 0, _screen.width, _screen.height});
        return Error();
}

Error GuiWindow::show(const Placement &placement) {
        // On the work area (a setting saved on a larger screen, or with a panel elsewhere).
        const Rect work = _app.windowManager().workArea();
        Rect       r = placement.normal;
        if (r.width == 0 || r.height == 0) r = _config.rect;
        if (work.width != 0 && work.height != 0) {
                if (r.width > work.width) r.width = work.width;
                if (r.height > work.height) r.height = work.height;
                if (r.x < work.x) r.x = work.x;
                if (r.y < work.y) r.y = work.y;
                if (r.x + r.width > work.x + work.width) r.x = uint16_t(work.x + work.width - r.width);
                if (r.y + r.height > work.y + work.height) r.y = uint16_t(work.y + work.height - r.height);
        }
        const Error err = show(r);
        if (err.isError() || !placement.maximized) return err;
        (void)_app.windowManager().maximize(id());
        return Error::Ok;
}

GuiWindow::Placement GuiWindow::placement() const {
        const uint8_t id = this->id();
        if (id != 0) {
                auto [placed, err] = _app.windowManager().placement(id);
                if (err.isOk()) return placed;
        }
        return _lastPlacement;
}

void GuiWindow::close() {
        const uint8_t id = _id.exchange(0, std::memory_order_acq_rel);
        if (id != 0) {
                auto [placed, err] = _app.windowManager().placement(id);
                if (err.isOk()) _lastPlacement = placed;
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

Error GuiWindow::setLevel(WindowManager::Level level, uint16_t reserve) {
        if (isShown()) return Error::InvalidState;
        _config.level = level;
        _config.reserve = reserve;
        _config.focusOnPress = level != WindowManager::Level::Panel; // a press on a panel keeps the focus where it is
        return Error::Ok;
}

Error GuiWindow::setDecorations(uint8_t decorations) {
        if (!WindowFrame::isValid(decorations)) return Error::Invalid;
        const uint8_t id = this->id();
        if (id != 0) {
                const Error err = _app.windowManager().setDecorations(id, decorations);
                if (err.isError()) return err;
        }
        _config.decorations = decorations;
        return Error::Ok;
}

void GuiWindow::setPointer(WindowManager::Pointer shape) {
        _forcedPointer = shape;
        const uint8_t id = this->id();
        if (id == 0) return;
        // Arrow: the widget under the pointer says again at its next move.
        _pointer = shape;
        (void)_app.windowManager().setPointer(id, shape);
        return;
}

void GuiWindow::setMinimumSize(uint16_t width, uint16_t height) {
        _minWidth = width;
        _minHeight = height;
        const uint8_t id = this->id();
        if (id != 0) (void)_app.windowManager().setMinimumSize(id, width, height);
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
        _app.forgetDragSource(widget);
        // Its children are being destroyed too: anything inside it.
        if (_default != nullptr) {
                for (Widget *p = _default; p != nullptr; p = p->parent()) {
                        if (p == widget) {
                                _default = nullptr;
                                break;
                        }
                }
        }
        for (Widget **w : {&_grab, &_hover, &_focus, &_central, &_dropWidget}) {
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

void GuiWindow::closedEntry(void *arg, const WindowManager::Placement &placement) {
        GuiApplication::Event e;
        e.type = Type::Closed;
        e.x = placement.normal.x;
        e.y = placement.normal.y;
        e.width = placement.normal.width;
        e.height = placement.normal.height;
        e.maximized = placement.maximized;
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

void GuiWindow::closeRequestedEntry(void *arg) {
        GuiApplication::Event e;
        e.type = Type::CloseRequested;
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

void GuiWindow::dropEntry(void *arg, const WindowManager::DropEvent &event) {
        GuiApplication::Event e;
        e.type = Type::Drop;
        e.drop = event;
        static_cast<GuiWindow *>(arg)->queue(e);
        return;
}

// ---- Drag and drop (devplan/clipboard.md §5d)

Result<uint32_t> GuiWindow::startDrag(const GuiDrag &drag, Widget *source) {
        const uint8_t id = this->id();
        if (id == 0) return makeError<uint32_t>(Error::InvalidState);
        GuiClipboard *c = _app.clipboard();
        if (c == nullptr) return makeError<uint32_t>(Error::NotExist);
        const Error err = c->copy(Clipboard::Selection::Drag, drag.types, drag.provider, drag.providerArg);
        if (err.isError()) return makeError<uint32_t>(err);
        WindowManager::DragConfig dc;
        dc.offer = c->serial(Clipboard::Selection::Drag);
        dc.actions = drag.actions;
        dc.preferred = drag.preferred;
        dc.image = drag.image;
        dc.width = drag.width;
        dc.height = drag.height;
        dc.hotX = drag.hotX;
        dc.hotY = drag.hotY;
        dc.ended = GuiApplication::dragEndedEntry;
        dc.arg = &_app;
        auto [number, serr] = _app.windowManager().startDrag(id, dc);
        if (serr.isError()) {
                (void)c->clear(Clipboard::Selection::Drag);
                return makeError<uint32_t>(serr);
        }
        _dragId = number;
        _dragEnded = drag.ended;
        _dragEndedArg = drag.endedArg;
        _app.beginDrag(number, source, *this);
        return makeResult(number);
}

// This window's drag @p drag over: the offer withdrawn, the source told.
void GuiWindow::dragEnded(uint32_t drag, DropAction done) {
        if (drag == 0 || drag != _dragId) return;
        _dragId = 0;
        GuiClipboard *c = _app.clipboard();
        if (c != nullptr) (void)c->clear(Clipboard::Selection::Drag);
        const GuiDrag::Ended fn = _dragEnded;
        void                *arg = _dragEndedArg;
        _dragEnded = nullptr;
        _app.beginDrag(0, nullptr, *this);
        if (fn != nullptr) fn(arg, done);
        return;
}

// A drag over the window: to the deepest widget under it that takes drags (entered, moved, left as
// it changes); its answer to the window manager.
void GuiWindow::dropEvent(const WindowManager::DropEvent &event) {
        using DType = WindowManager::DropEvent::Type;
        GuiDropEvent e(*this, event);
        Widget      *target = nullptr;
        if (event.type != DType::Leave)
                for (Widget *p = widgetAt(event.x, event.y); p != nullptr && target == nullptr; p = p->parent())
                        if (p->takesDrags() && p->isVisible() && p->isEnabled()) target = p;
        if (target != _dropWidget && _dropWidget != nullptr) _dropWidget->dragLeft();
        if (event.type == DType::Leave) {
                _dropWidget = nullptr;
                return;
        }
        if (event.type == DType::Drop) {
                if (target != nullptr && target == _dropWidget) target->dragLeft(); // its marks gone first
                _dropWidget = nullptr;
                const bool taken = target != nullptr && (target->askDropHook(e) || target->dropped(e));
                if (!taken) (void)_app.windowManager().finishDrop(event.drag, DropAction::None);
                return;
        }
        if (target != nullptr && !target->askDropHook(e)) {
                if (target == _dropWidget) {
                        target->dragMoved(e);
                } else {
                        target->dragEntered(e);
                }
        }
        _dropWidget = target;
        (void)_app.windowManager().answerDrop(event.drag, id(), target != nullptr ? e.answer() : DropAction::None);
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
                case Type::Drop: dropEvent(event.drop); break;
                case Type::DragEnded: dragEnded(event.drop.drag, static_cast<DropAction>(event.width)); break;
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
                                if (event.width != 0) {
                                        _lastPlacement.normal = Rect{event.x, event.y, event.width, event.height};
                                        _lastPlacement.maximized = event.maximized;
                                }
                                _screen = Rect();
                                _grab = _hover = nullptr;
                                if (_dropWidget != nullptr) _dropWidget->dragLeft();
                                _dropWidget = nullptr;
                                _active = false;
                                _dirty.clear();
                                closedByManager();
                        }
                        break;
                case Type::CloseRequested:
                        if (isShown()) closeRequested();
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

void GuiWindow::closeRequested() {
        close();
        closedByManager();
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
                        // A right press: a context menu, if the widget or one around it takes it.
                        bool taken = false;
                        if (event.button == PointerEvent::Right && w->isEnabled()) {
                                Widget::InputHookEvent ask;
                                ask.x = event.x;
                                ask.y = event.y;
                                for (Widget *c = w; c != nullptr && !taken; c = c->parent())
                                        taken = c->askInputHook(ask);
                        }
                        if (!taken && w->isEnabled()) w->mousePressed(event);
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
        Widget::InputHookEvent ask;
        ask.type = Widget::InputHookEvent::Type::Wheel;
        ask.notches = notches;
        for (Widget *w = _hover; w != nullptr; w = w->parent())
                if (w->isVisible() && w->isEnabled() && (w->askInputHook(ask) || w->wheelTurned(notches))) return;
        return;
}

WindowManager::Pointer GuiWindow::pointerAt(int x, int y) const {
        (void)x;
        (void)y;
        return _config.pointer;
}

void GuiWindow::pointerOver(Widget *widget, int x, int y) {
        const WindowManager::Pointer shape =
                _forcedPointer != WindowManager::Pointer::Arrow ? _forcedPointer : widget->pointerAt(x, y);
        if (shape == _pointer || !isShown()) return;
        _pointer = shape;
        (void)_app.windowManager().setPointer(id(), shape);
        return;
}

BMDS_NAMESPACE_END
