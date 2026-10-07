/**
 * @file      windowmanager.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop and its windows, drawn directly on surface 0
 * (devplan/windows.md §4a).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/deadline.h>
#include <bmds/display.h>
#include <bmds/font.h>
#include <bmds/input.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/surface.h>
#include <bmds/system.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr size_t   StripAlign = 64; // the L1 cache line
        constexpr uint16_t Align = WindowStack::Align;

        constinit StaticObject<WindowManager> managers[WindowManager::MaxManagers];

        // @p title into @p out (MaxTitle bytes at most, cut on a character's boundary), terminated.
        void copyTitle(char (&out)[WindowManager::MaxTitle + 1], StringView title) {
                size_t n = title.size() < WindowManager::MaxTitle ? title.size() : WindowManager::MaxTitle;
                if (n < title.size())
                        while (n > 0 && (static_cast<uint8_t>(title[n]) & 0xc0u) == 0x80u) --n;
                std::memcpy(out, title.data(), n);
                out[n] = '\0';
                return;
        }

} // namespace

Result<WindowManager *> WindowManager::manager(size_t number) {
        if (number >= MaxManagers) return makeError<WindowManager *>(Error::NotSupported);
        if (!managers[number].isCreated()) managers[number].create();
        return makeResult(&*managers[number]);
}

Error WindowManager::start(Display &display, const WindowPalette &palette) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface != nullptr || !display.isRunning()) return Error::InvalidState;
        Surface &surface = display.surface();
        // A text row at the screen's width in two halves (each a multiple of 64
        // pixels: its own cache lines), and a round's plan and clips.
        const uint16_t half = static_cast<uint16_t>((surface.width() / 2 + 63) & ~63u);
        const size_t   stripBytes = 2 * size_t(half) * Surface::BytesPerPixel * Fonts::unscii16.height();
        _tools.strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, stripBytes, StripAlign));
        _pieces = static_cast<ComposePiece *>(Memory::alloc(MemSpace::Internal, MaxPieces * sizeof(ComposePiece)));
        _scratch = static_cast<Rect *>(Memory::alloc(MemSpace::Internal, 3 * MaxPieces * sizeof(Rect)));
        Error err = _tools.strip == nullptr || _pieces == nullptr || _scratch == nullptr ? Error(Error::NoMem)
                                                                                         : display.claim("desktop");
        if (err.isOk() && !_task.isRunning()) {
                Task::Config tc;
                tc.name = "wm";
                tc.stackBytes = 8192;
                tc.priority = 5;
                tc.core = 0;
                err = _task.start(tc, entry, this);
                if (err.isError()) display.release();
        }
        if (err.isError()) {
                Memory::free(_tools.strip);
                Memory::free(_pieces);
                Memory::free(_scratch);
                _tools = Painter::Tools();
                _pieces = nullptr;
                _scratch = nullptr;
                return err;
        }
        _tools.halfWidth = half;
        _tools.height = Fonts::unscii16.height();
        _tools.renderer = &_renderer;
        _tools.palette = &_palette;
        _tools.busyHalf = -1;
        _updates = 0;
        _display = &display;
        _surface = &surface;
        _palette = palette;
        _stack.reset(surface.width(), surface.height()); // all damaged: the desktop drawn
        for (Window &w : _windows) w = Window();
        _dragId = 0;
        _outlineShown = false;
        _stats = Stats();
        {
                SpinLock::Locker keys(_inputLock);
                _keyCount = 0;
                _pointerCount = 0;
        }
        _pointerX = static_cast<uint16_t>(surface.width() / 2);
        _pointerY = static_cast<uint16_t>(surface.height() / 2);
        _buttons = 0;
        _cursorShown = false;
        _cursorMoved = false;
        _grabId = 0;
        _pressId = 0;
        _pressShown = false;
        _clickId = 0;
        _sizeId = 0;
        _shape = Pointer::Arrow;
        _pointerHidden = false;
        _typingHidden = false;
        _mouseId = 0;
        _hoverId = 0;
        _swallowed = 0;
        _listChanged = false;
        _workArea = _stack.screen();
        _menuId = 0;
        _menuFor = 0;
        _menuHighlight = -1;
        _focusId = 0;
        // The keyboards' and mice's sink while it runs.
        Input::Sink sink;
        sink.key = keyEntry;
        sink.pointer = pointerEntry;
        sink.arg = this;
        Input::setSink(&sink);
        _requests.fetch_add(1, std::memory_order_acq_rel);
        _wake.release();
        return Error::Ok;
}

void WindowManager::stop() {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return;
        Input::setSink(nullptr);
        for (size_t i = 0; i < _stack.count() + _stack.hiddenCount(); ++i) {
                Window &w = _windows[(i < _stack.count() ? _stack.idAt(i) : _stack.hiddenIdAt(i - _stack.count())) - 1];
                if (w.closed != nullptr) w.closed(w.arg);
                w = Window();
        }
        // The applications told: their windows are gone, and so is the window manager.
        for (Client &c : _clients) {
                if (c.used && c.gone != nullptr) c.gone(c.arg);
                c = Client();
        }
        (void)_surface->wait(DrawTimeoutMs); // the strip's last copy
        _surface = nullptr;
        _display->release();
        _display = nullptr;
        Memory::free(_tools.strip);
        Memory::free(_pieces);
        Memory::free(_scratch);
        _tools = Painter::Tools();
        _pieces = nullptr;
        _scratch = nullptr;
        _stack.reset(0, 0);
        _dragId = 0;
        _asked.store(0, std::memory_order_release);
        _drawn.store(_requests.load(std::memory_order_acquire), std::memory_order_release); // nothing more will be
        _idle.notify();
        return;
}

void WindowManager::entry(void *arg) {
        static_cast<WindowManager *>(arg)->run();
        return;
}

void WindowManager::run() {
        for (;;) {
                (void)_wake.acquire(WaitForever);
                Surface *surface = nullptr;
                uint32_t frame = 0;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        surface = _surface;
                        if (surface != nullptr) {
                                // Counted before the keys are taken: a key queued later is not
                                // reported drawn (sync()) before it is delivered.
                                const uint32_t requests = _requests.load(std::memory_order_acquire);
                                deliverKeys();
                                applyPointer();
                                frame = surface->frames();
                                round(requests);
                                if (_listChanged) {
                                        _listChanged = false;
                                        for (const Client &c : _clients)
                                                if (c.used && c.watch != nullptr) c.watch(c.watchArg);
                                }
                        }
                }
                _idle.notify();
                // A round a frame at most: what comes meanwhile is drawn together.
                if (surface != nullptr) (void)surface->waitFrame(frame, DrawTimeoutMs);
        }
}

void WindowManager::keyEntry(void *arg, const KeyEvent &event) {
        static_cast<WindowManager *>(arg)->key(event);
        return;
}

void WindowManager::key(const KeyEvent &event) {
        keyTo(0, event);
        return;
}

void WindowManager::keyTo(uint8_t id, const KeyEvent &event) {
        {
                SpinLock::Locker keys(_inputLock);
                if (_keyCount == KeyQueue) return; // nobody is taking them
                const size_t at = (_keyHead + _keyCount) % KeyQueue;
                _keys[at] = event;
                _keyTargets[at] = id;
                ++_keyCount;
        }
        _requests.fetch_add(1, std::memory_order_acq_rel); // a change: sync() waits for its delivery
        _wake.release();
        return;
}

// The keys queued, to the focused window. With the lock held.
void WindowManager::deliverKeys() {
        for (;;) {
                KeyEvent event;
                uint8_t  target = 0;
                {
                        SpinLock::Locker keys(_inputLock);
                        if (_keyCount == 0) return;
                        event = _keys[_keyHead];
                        target = _keyTargets[_keyHead];
                        _keyHead = static_cast<uint8_t>((_keyHead + 1) % KeyQueue);
                        --_keyCount;
                }
                // The window manager's own keys first (not one sent to a chosen window: a test's).
                _modifiers = event.modifiers;
                if (target == 0 && shortcut(event)) continue;
                // To its window if it names one (still open), else an open popup, else the focus.
                if (target != 0 && _stack.rectOf(target).isError()) continue;
                if (target == 0) target = topPopup();
                if (target == 0) target = focusId();
                if (target == 0) continue;
                const Window &w = _windows[target - 1];
                // Typing hides the pointer from a window asking (not a modifier or a lock alone).
                const bool typing = event.action != KeyEvent::Action::Release && event.usage < 0xe0 &&
                                    event.usage != HidKey::CapsLock && event.usage != HidKey::NumLock &&
                                    event.usage != HidKey::ScrollLock;
                if (typing && w.hideWhileTyping && !_typingHidden) {
                        if (isCursorDrawn()) _stack.invalidateScreen(cursorArea());
                        _typingHidden = true;
                }
                if (w.key != nullptr) w.key(w.arg, event);
        }
}

void WindowManager::pointerEntry(void *arg, const PointerEvent &event) {
        static_cast<WindowManager *>(arg)->pointer(event);
        return;
}

void WindowManager::pointer(const PointerEvent &event) {
        {
                SpinLock::Locker keys(_inputLock);
                PointerEvent    *last =
                        _pointerCount > 0 ? &_pointers[(_pointerHead + _pointerCount - 1) % PointerQueue] : nullptr;
                if (last != nullptr && last->buttons == event.buttons) {
                        // Motion alone: merged, so a fast mouse costs one event a round.
                        last->dx = static_cast<int16_t>(last->dx + event.dx);
                        last->dy = static_cast<int16_t>(last->dy + event.dy);
                        last->wheel = static_cast<int8_t>(last->wheel + event.wheel);
                } else if (_pointerCount < PointerQueue) {
                        _pointers[(_pointerHead + _pointerCount) % PointerQueue] = event;
                        ++_pointerCount;
                } else {
                        return; // nobody is taking them
                }
        }
        _requests.fetch_add(1, std::memory_order_acq_rel); // a change: sync() waits for it to be applied
        _wake.release();
        return;
}

// The pointer's events queued: moved, buttons pressed and released. With the lock held.
void WindowManager::applyPointer() {
        for (;;) {
                PointerEvent event;
                {
                        SpinLock::Locker keys(_inputLock);
                        if (_pointerCount == 0) return;
                        event = _pointers[_pointerHead];
                        _pointerHead = static_cast<uint8_t>((_pointerHead + 1) % PointerQueue);
                        --_pointerCount;
                }
                const Rect screen = _stack.screen();
                const int  x = _pointerX + event.dx, y = _pointerY + event.dy;
                const auto nx = static_cast<uint16_t>(x < 0 ? 0 : x >= screen.width ? screen.width - 1 : x);
                const auto ny = static_cast<uint16_t>(y < 0 ? 0 : y >= screen.height ? screen.height - 1 : y);
                const Rect was = cursorArea();
                const bool wasDrawn = isCursorDrawn();
                const bool moved = nx != _pointerX || ny != _pointerY;
                if (moved || event.buttons != _buttons || event.wheel != 0) _typingHidden = false; // the mouse is back
                if (moved) {
                        _pointerX = nx;
                        _pointerY = ny;
                        if (_grabId != 0) {
                                const int wx = int(nx) - _grabX, wy = int(ny) - _grabY;
                                (void)dragLocked(_grabId, static_cast<uint16_t>(wx < 0 ? 0 : wx),
                                                 static_cast<uint16_t>(wy < 0 ? 0 : wy));
                        }
                        if (_sizeId != 0) sizeTo(nx, ny);
                        if (_pressId != 0) pressedMoved(nx, ny);
                }
                const uint8_t pressed = static_cast<uint8_t>(event.buttons & ~_buttons);
                const uint8_t released = static_cast<uint8_t>(_buttons & ~event.buttons);
                _buttons = event.buttons;
                if (moved) {
                        if (_mouseId != 0) {
                                mouseTo(_mouseId, MouseEvent::Type::Motion, nx, ny, 0);
                        } else {
                                hoverAt(nx, ny);
                        }
                }
                for (const uint8_t b : {PointerEvent::Left, PointerEvent::Right, PointerEvent::Middle}) {
                        if ((pressed & b) == 0) continue;
                        // A press off every popup closes them, and goes no further.
                        if (topPopup() != 0) {
                                const uint8_t at = windowAt(nx, ny);
                                if (at == 0 || _stack.levelOf(at) != Level::Popup) {
                                        closePopups();
                                        _swallowed = static_cast<uint8_t>(_swallowed | b);
                                        continue;
                                }
                        }
                        press(nx, ny, b);
                }
                for (const uint8_t b : {PointerEvent::Left, PointerEvent::Right, PointerEvent::Middle}) {
                        if ((released & b) == 0) continue;
                        if ((_swallowed & b) != 0) {
                                _swallowed = static_cast<uint8_t>(_swallowed & ~b);
                                continue;
                        }
                        if (b == PointerEvent::Left) release(nx, ny);
                        if (_mouseId != 0) {
                                mouseTo(_mouseId, MouseEvent::Type::Release, nx, ny, b);
                        } else if (_hoverId != 0) {
                                // Pressed elsewhere (a menu's button), released here: a menu's choice.
                                mouseTo(_hoverId, MouseEvent::Type::Release, nx, ny, b);
                        }
                }
                if (_mouseId != 0 && _buttons == 0) {
                        _mouseId = 0;
                        hoverAt(nx, ny);
                }
                if (event.wheel != 0) {
                        // To the window under the pointer, focused or not.
                        const uint8_t id = windowAt(nx, ny);
                        const Window *w = id != 0 ? &_windows[id - 1] : nullptr;
                        if (w != nullptr && w->wheel != nullptr) w->wheel(w->arg, event.wheel);
                }
                const Pointer shape = shapeAt(nx, ny);
                const bool    changed = moved || shape != _shape;
                _shape = shape;
                _cursorShown = true;
                const bool drawn = isCursorDrawn();
                if (wasDrawn && (changed || !drawn)) _stack.invalidateScreen(was); // what it covered, redrawn
                if (drawn && (changed || !wasDrawn)) _cursorMoved = true;
        }
}

WindowFrame WindowManager::frame(uint8_t id, const Rect &outer) const {
        return WindowFrame(outer, _windows[id - 1].decorations);
}

// Button @p button down at (@p x, @p y): the window there raised; in its
// client area, the press its (with the mouse until every button is up). The
// left button on a frame's button presses it (it acts if released there; the
// menu's opens at once); on the title bar, grabs it to move, or maximizes or
// restores it with a double click. The right button on the title bar opens
// the window's menu.
void WindowManager::press(uint16_t x, uint16_t y, uint8_t button) {
        using Part = WindowFrame::Part;
        const uint8_t id = windowAt(x, y);
        if (id == 0) return;
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return;
        const WindowFrame f = frame(id, _stack.rectAt(z));
        const Part        part = f.partAt(x, y);
        (void)raiseLocked(id);
        if (button == PointerEvent::Left && (_modifiers & KeyEvent::Alt) != 0 &&
            WindowStack::isApplication(_stack.levelOf(id)) && !_windows[id - 1].maximized) {
                // Alt and a press anywhere in it: moved, as by its title bar.
                _clickId = 0;
                _grabId = id;
                _grabX = static_cast<uint16_t>(x - f.outer.x);
                _grabY = static_cast<uint16_t>(y - f.outer.y);
                return;
        }
        if (part == Part::Client) {
                _clickId = 0;
                if (_windows[id - 1].mouse == nullptr) return;
                if (_mouseId == 0) _mouseId = id;
                if (_mouseId == id) mouseTo(id, MouseEvent::Type::Press, x, y, button);
                return;
        }
        if (button != PointerEvent::Left) {
                if (button == PointerEvent::Right && part == Part::TitleBar)
                        (void)showWindowMenuLocked(id, Rect{x, y, 0, 0});
                return;
        }
        if (part == Part::Menu) {
                _clickId = 0;
                (void)showWindowMenuLocked(id, f.button(part));
                return;
        }
        const uint8_t edges = WindowFrame::edgesOf(part);
        if (edges != 0) {
                _clickId = 0;
                if (_windows[id - 1].maximized) return; // sized with the screen
                _sizeId = id;
                _sizeEdges = edges;
                _sizeStart = f.outer;
                _sizeX = x;
                _sizeY = y;
                return;
        }
        if (part == Part::Minimize || part == Part::Maximize || part == Part::Close) {
                _pressId = id;
                _pressPart = part;
                _pressShown = true;
                (void)_stack.invalidate(id, f.button(part));
                _clickId = 0;
                return;
        }
        if (part != Part::TitleBar) {
                _clickId = 0;
                return;
        }
        const uint64_t now = System::uptimeUs();
        if (_clickId == id && now - _clickUs < uint64_t(DoubleClickMs) * 1000) {
                _clickId = 0;
                if (f.has(WindowFrame::MaximizeButton))
                        (void)(_windows[id - 1].maximized ? restoreLocked(id) : maximizeLocked(id));
                return;
        }
        _clickId = id;
        _clickUs = now;
        if (_windows[id - 1].maximized) return; // stays where it is
        _grabId = id;
        _grabX = static_cast<uint16_t>(x - f.outer.x);
        _grabY = static_cast<uint16_t>(y - f.outer.y);
        return;
}

void WindowManager::release(uint16_t x, uint16_t y) {
        if (_grabId != 0) {
                if (_dragId == _grabId) (void)dropLocked(); // moved: put there
                _grabId = 0;
        }
        if (_sizeId != 0) {
                if (_dragId == _sizeId) (void)dropLocked(); // resized: put there, told its size
                _sizeId = 0;
        }
        if (_pressId != 0) {
                const uint8_t           id = _pressId;
                const WindowFrame::Part part = _pressPart;
                pressedMoved(x, y);
                const bool over = _pressShown;
                _pressId = 0;
                _pressShown = false;
                auto [z, err] = _stack.layerOf(id);
                if (err.isOk()) (void)_stack.invalidate(id, frame(id, _stack.rectAt(z)).button(part));
                if (err.isOk() && over) act(id, part);
        }
        return;
}

// The pointer moved with a button pressed: drawn pressed while over it.
void WindowManager::pressedMoved(uint16_t x, uint16_t y) {
        auto [z, err] = _stack.layerOf(_pressId);
        if (err.isError()) {
                _pressId = 0;
                return;
        }
        const WindowFrame f = frame(_pressId, _stack.rectAt(z));
        const bool        over = windowAt(x, y) == _pressId && f.partAt(x, y) == _pressPart;
        if (over == _pressShown) return;
        _pressShown = over;
        (void)_stack.invalidate(_pressId, f.button(_pressPart));
        return;
}

// A resize under way, the pointer at (@p x, @p y): the outline where the window would be.
void WindowManager::sizeTo(uint16_t x, uint16_t y) {
        const Window &w = _windows[_sizeId - 1];
        const Rect    area = WindowStack::isApplication(_stack.levelOf(_sizeId)) ? _workArea : _stack.screen();
        const Rect    to = WindowFrame::resizedRect(_sizeStart, _sizeEdges, int(x) - _sizeX, int(y) - _sizeY,
                                                    w.decorations, w.widthStep, w.heightStep, area);
        (void)outlineLocked(_sizeId, to);
        return;
}

// The pointer's picture at (@p x, @p y): the resize's under way, a handle's, or the arrow.
WindowManager::Pointer WindowManager::shapeAt(uint16_t x, uint16_t y) const {
        uint8_t edges = _sizeEdges;
        if (_sizeId == 0) {
                if (_grabId != 0 || _pressId != 0) return Pointer::Arrow;
                const uint8_t id = windowAt(x, y);
                if (id == 0) return Pointer::Arrow;
                auto [z, err] = _stack.layerOf(id);
                if (err.isError()) return Pointer::Arrow;
                const Window           &w = _windows[id - 1];
                const WindowFrame::Part part = frame(id, _stack.rectAt(z)).partAt(x, y);
                if (part == WindowFrame::Part::Client) return w.pointer;
                if (w.maximized) return Pointer::Arrow; // sized with the screen
                edges = WindowFrame::edgesOf(part);
        }
        constexpr uint8_t Horizontal = WindowFrame::LeftEdge | WindowFrame::RightEdge;
        constexpr uint8_t Vertical = WindowFrame::TopEdge | WindowFrame::BottomEdge;
        if (edges == 0) return Pointer::Arrow;
        if ((edges & Vertical) == 0) return Pointer::Horizontal;
        if ((edges & Horizontal) == 0) return Pointer::Vertical;
        const bool down = edges == (WindowFrame::TopEdge | WindowFrame::LeftEdge) ||
                          edges == (WindowFrame::BottomEdge | WindowFrame::RightEdge);
        return down ? Pointer::DiagonalDown : Pointer::DiagonalUp;
}

// What a title-bar button does (the menu's opens at the press).
void WindowManager::act(uint8_t id, WindowFrame::Part part) {
        using Part = WindowFrame::Part;
        if (part == Part::Close) {
                (void)closeLocked(id);
        } else if (part == Part::Minimize) {
                (void)minimizeLocked(id);
        } else if (part == Part::Maximize) {
                (void)(_windows[id - 1].maximized ? restoreLocked(id) : maximizeLocked(id));
        }
        return;
}

uint8_t WindowManager::windowAt(uint16_t x, uint16_t y) const {
        const Rect at{x, y, 1, 1};
        for (size_t z = 0; z < _stack.count(); ++z)
                if (_stack.rectAt(z).contains(at)) return _stack.idAt(z);
        return 0;
}

Error WindowManager::capture(Capture fn, void *arg, unsigned int timeoutMs) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_surface->buffering() == Surface::Buffering::Double) {
                const Error err = _surface->waitShown(_presented, timeoutMs);
                if (err.isError()) return err;
        }
        fn(arg, *_surface);
        return Error::Ok;
}

Rect WindowManager::pointerArea() {
        Mutex::Locker lock(_lock, WaitForever);
        return _surface != nullptr && isCursorDrawn() ? cursorArea() : Rect();
}

void WindowManager::setPointerVisible(bool visible) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr || _pointerHidden == !visible) return;
        const bool wasDrawn = isCursorDrawn();
        if (wasDrawn) _stack.invalidateScreen(cursorArea());
        _pointerHidden = !visible;
        if (!wasDrawn && isCursorDrawn()) _cursorMoved = true;
        changed(focusId());
        return;
}

bool WindowManager::isPointerVisible() {
        Mutex::Locker lock(_lock, WaitForever);
        return !_pointerHidden;
}

namespace {

        // A pointer's picture: 'X' its edge, 'o' its fill, '.' what shows
        // through; the hot spot is where the pointer is.
        struct Picture {
                        uint8_t            width, height, hotX, hotY;
                        const char *const *rows;
        };

        constexpr const char *ArrowRows[] = {
                "X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......",
                "XooooX......", "XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..",
                "XoooooooooX.", "XooooooXXXXX", "XoooXooX....", "XooXXooX....", "XoX..XooX...",
                "XX...XooX...", "X.....XooX..", "......XooX..", ".......XX...",
        };

        constexpr const char *HorizontalRows[] = {
                "...XXX.......XXX...", "..XXoX.......XoXX..", ".XXooX.......XooXX.",
                "XXoooXXXXXXXXXoooXX", "XoooooooooooooooooX", "XXoooXXXXXXXXXoooXX",
                ".XXooX.......XooXX.", "..XXoX.......XoXX..", "...XXX.......XXX...",
        };

        constexpr const char *VerticalRows[] = {
                "...XXX...", "..XXoXX..", ".XXoooXX.", "XXoooooXX", "XoooooooX", "XXXXoXXXX", "...XoX...",
                "...XoX...", "...XoX...", "...XoX...", "...XoX...", "...XoX...", "...XoX...", "XXXXoXXXX",
                "XoooooooX", "XXoooooXX", ".XXoooXX.", "..XXoXX..", "...XXX...",
        };

        constexpr const char *DiagonalDownRows[] = {
                "XXXXXXX........", "XoooooX........", "XooooXX........", "XoooXX.........", "XooXoXX........",
                "XoXXXoXX.......", "XXX.XXoXX......", ".....XXoXX.....", "......XXoXX.XXX", ".......XXoXXXoX",
                "........XXoXooX", ".........XXoooX", "........XXooooX", "........XoooooX", "........XXXXXXX",
        };

        constexpr const char *TextRows[] = {
                "XXX.XXX", "XooXooX", "XXXoXXX", "..XoX..", "..XoX..", "..XoX..", "..XoX..", "..XoX..", "..XoX..",
                "..XoX..", "..XoX..", "..XoX..", "..XoX..", "..XoX..", "XXXoXXX", "XooXooX", "XXX.XXX",
        };

        constexpr const char *DiagonalUpRows[] = {
                "........XXXXXXX", "........XoooooX", "........XXooooX", ".........XXoooX", "........XXoXooX",
                ".......XXoXXXoX", "......XXoXX.XXX", ".....XXoXX.....", "XXX.XXoXX......", "XoXXXoXX.......",
                "XooXoXX........", "XoooXX.........", "XooooXX........", "XoooooX........", "XXXXXXX........",
        };

        // By WindowManager::Pointer.
        constexpr Picture Pictures[] = {
                {12, 19, 0, 0, ArrowRows},        {19, 9, 9, 4, HorizontalRows},  {9, 19, 4, 9, VerticalRows},
                {15, 15, 7, 7, DiagonalDownRows}, {15, 15, 7, 7, DiagonalUpRows}, {7, 17, 3, 8, TextRows},
        };

        struct CursorJob {
                        const Picture *picture;
                        int            column; // the picture's first column, from the area's
                        int            row;    // and its first row
                        uint16_t       width;  // the area's
                        uint16_t       rows;
                        uint32_t       edge;
                        uint32_t       fill;
        };

} // namespace

// The pointer's picture on the screen, widened to 4-pixel columns.
Rect WindowManager::cursorArea() const {
        const Picture &p = Pictures[static_cast<size_t>(_shape)];
        const Rect     screen = _stack.screen();
        const int      x = int(_pointerX) - p.hotX, y = int(_pointerY) - p.hotY;
        const uint32_t left = uint32_t(x < 0 ? 0 : x) & ~(Align - 1u);
        const uint32_t top = uint32_t(y < 0 ? 0 : y);
        uint32_t       right = (uint32_t(x + p.width) + Align - 1u) & ~(Align - 1u);
        uint32_t       bottom = uint32_t(y + p.height);
        if (right > screen.width) right = screen.width;
        if (bottom > screen.height) bottom = screen.height;
        return Rect{static_cast<uint16_t>(left), static_cast<uint16_t>(top), static_cast<uint16_t>(right - left),
                    static_cast<uint16_t>(bottom - top)};
}

// The picture over what @p pixels holds (the cursor area).
void WindowManager::drawCursorPixels(void *arg, uint8_t *pixels, size_t stride) {
        const auto    *job = static_cast<const CursorJob *>(arg);
        const Picture &p = *job->picture;
        for (uint16_t r = 0; r < job->rows; ++r) {
                const int sr = int(r) - job->row; // the picture's row
                if (sr < 0 || sr >= p.height) continue;
                const char *shape = p.rows[sr];
                uint8_t    *row = pixels + size_t(r) * stride;
                for (uint16_t c = 0; c < job->width; ++c) {
                        const int sc = int(c) - job->column; // the picture's column
                        if (sc < 0 || sc >= p.width || shape[sc] == '.') continue;
                        const uint32_t rgb = shape[sc] == 'X' ? job->edge : job->fill;
                        uint8_t       *px = row + size_t(c) * Surface::BytesPerPixel;
                        px[0] = static_cast<uint8_t>(rgb);
                        px[1] = static_cast<uint8_t>(rgb >> 8);
                        px[2] = static_cast<uint8_t>(rgb >> 16);
                }
        }
        return;
}

// The pointer over everything drawn this round (drawing it again where it was is harmless).
void WindowManager::drawCursor() {
        _cursorMoved = false;
        if (!isCursorDrawn()) return;
        const Picture &p = Pictures[static_cast<size_t>(_shape)];
        const Rect     area = cursorArea();
        CursorJob      job;
        job.picture = &p;
        job.column = int(_pointerX) - p.hotX - area.x;
        job.row = int(_pointerY) - p.hotY - area.y;
        job.width = area.width;
        job.rows = area.height;
        job.edge = _palette.color(Role::CursorEdge);
        job.fill = _palette.color(Role::CursorFill);
        ++_stats.calls;
        if (_surface->patch(area, drawCursorPixels, &job, DrawTimeoutMs).isError()) ++_stats.errors;
        return;
}

// Everything damaged drawn by whoever shows it, the outline over it; then
// (double-buffered) presented. With the lock held.
// The changes counted up to @p requests are drawn by it.
void WindowManager::round(uint32_t requests) {
        _updates = static_cast<uint16_t>(_updates | _asked.exchange(0, std::memory_order_acq_rel)); // after the count
        const Region damage = _stack.damage();
        _stack.clearDamage();
        const bool drawing = !damage.isEmpty() || _updates != 0 || (_dragId != 0 && !_outlineShown) || _cursorMoved;
        if (!drawing) {
                _drawn.store(requests, std::memory_order_release);
                return;
        }
        const uint64_t start = System::uptimeUs();
        if (!damage.isEmpty()) {
                auto [n, err] = _stack.plan(damage, Span<ComposePiece>(_pieces, MaxPieces));
                if (err.isOk()) {
                        drawPieces(Span<const ComposePiece>(_pieces, n));
                } else {
                        // Too broken up: a rectangle at a time, or bottom to top.
                        ++_stats.fallbacks;
                        for (const Rect &rect : damage.rects()) {
                                Region one;
                                one.add(rect);
                                auto [m, merr] = _stack.plan(one, Span<ComposePiece>(_pieces, MaxPieces));
                                if (merr.isOk()) {
                                        drawPieces(Span<const ComposePiece>(_pieces, m));
                                } else {
                                        drawBottomUp(rect);
                                }
                        }
                }
        }
        drawUpdates();
        if (_dragId != 0) drawOutline();
        drawCursor();
        const auto us = static_cast<uint32_t>(System::uptimeUs() - start);
        ++_stats.rounds;
        _stats.roundUs += us;
        _stats.lastRoundUs = us;
        if (us > _stats.roundUsMax) _stats.roundUsMax = us;
        if (_surface->buffering() == Surface::Buffering::Double) {
                auto [number, err] = _surface->present(DrawTimeoutMs);
                if (err.isOk()) {
                        _presented = number;
                } else {
                        ++_stats.errors;
                }
        }
        _drawn.store(requests, std::memory_order_release);
        return;
}

void WindowManager::drawPieces(Span<const ComposePiece> pieces) {
        _stats.pieces += static_cast<uint32_t>(pieces.size());
        const size_t windows = _stack.count();
        for (size_t z = 0; z <= windows; ++z) {
                size_t k = 0;
                for (const ComposePiece &piece : pieces)
                        if (piece.layer == z) _scratch[k++] = piece.rect;
                if (k == 0) continue;
                if (z == windows) {
                        // The desktop's: aligned and disjoint already.
                        ++_stats.calls;
                        if (_surface->fill(Span<const Rect>(_scratch, k), _palette.color(Role::Desktop), DrawTimeoutMs)
                                    .isError())
                                ++_stats.errors;
                } else {
                        drawLayer(z, Span<const Rect>(_scratch, k));
                }
        }
        return;
}

// Window @p z's frame and client area inside @p pieces (its damage); after
// `update()`, its redraw clipped to all of its client area that shows.
void WindowManager::drawLayer(size_t z, Span<const Rect> pieces) {
        const uint8_t     id = _stack.idAt(z);
        const Window     &w = _windows[id - 1];
        const Rect        outer = _stack.rectAt(z);
        const bool        active = id == focusId();
        const uint32_t    color = _palette.color(active ? Role::ActiveFrame : Role::InactiveFrame);
        const uint32_t    text = _palette.color(active ? Role::ActiveTitle : Role::InactiveTitle);
        const auto        bit = static_cast<uint16_t>(1u << (id - 1));
        const WindowFrame f = frame(id, outer);

        if (!pieces.isEmpty() && f.decorations != WindowFrame::None) {
                using Part = WindowFrame::Part;
                Painter chrome(*_surface, pieces, pieces, 0, 0, _tools, DrawTimeoutMs);
                Rect    edges[4];
                Rect    parts[5];
                size_t  n = f.edges(edges);
                for (size_t i = 0; i < n; ++i) parts[i] = edges[i];
                if (f.has(WindowFrame::TitleBar)) parts[n++] = f.titleBar();
                (void)chrome.fill(Span<const Rect>(parts, n), color);
                // Each button: an icon, a glyph as big as the button, raised by a bevel around it
                // (the glyphs leave the outer two pixels blank); pressed, sunk.
                struct Button {
                                Part     part;
                                Role     face, mark;
                                uint32_t icon;
                };
                const Button buttons[] = {
                        {Part::Menu, Role::Button, Role::ButtonMark, bmds::Icon::Menu},
                        {Part::Minimize, Role::Button, Role::ButtonMark, bmds::Icon::Minimize},
                        {Part::Maximize, Role::Button, Role::ButtonMark,
                         w.maximized ? bmds::Icon::Restore : bmds::Icon::Maximize},
                        {Part::Close, Role::CloseBox, Role::CloseMark, bmds::Icon::Close},
                };
                for (const Button &b : buttons) {
                        const Rect r = f.button(b.part);
                        if (r.isEmpty()) continue;
                        const bool pressed = _pressId == id && _pressPart == b.part && _pressShown;
                        TextCell   icon;
                        icon.codepoint = b.icon;
                        icon.fg = _palette.color(b.mark);
                        icon.bg = _palette.color(b.face);
                        (void)chrome.cells(r.x, r.y, Span<const TextCell>(&icon, 1), Fonts::icons16, r.width);
                        drawBevel(chrome, r, pressed ? Bevel::Style::Sunken : Bevel::Style::Raised);
                }
                const Rect title = f.title();
                if (!title.isEmpty())
                        (void)chrome.text(title.x, title.y, StringView(w.title), Fonts::unscii16, text, color,
                                          title.width);
                count(chrome);
        }

        // The client area's damage; its clip, the damage or (an update) all of it that shows.
        const Rect client = f.client();
        Rect      *damage = _scratch + MaxPieces;
        size_t     nd = 0;
        for (const Rect &piece : pieces) {
                const Rect r = piece.intersected(client);
                if (!r.isEmpty()) damage[nd++] = r;
        }
        Span<const Rect> clip(damage, nd);
        if ((_updates & bit) != 0) {
                // Planned in the third part of the scratch space, then kept there.
                Rect        *visible = _scratch + 2 * MaxPieces;
                auto        *plan = reinterpret_cast<ComposePiece *>(visible);
                const size_t room = MaxPieces * sizeof(Rect) / sizeof(ComposePiece);
                Region       area;
                area.add(client);
                auto [n, err] = _stack.plan(area, Span<ComposePiece>(plan, room));
                if (err.isOk()) {
                        size_t nv = 0;
                        for (size_t i = 0; i < n; ++i) {
                                const ComposePiece piece = plan[i]; // its slot may be overwritten below
                                if (piece.layer == z) visible[nv++] = piece.rect;
                        }
                        clip = Span<const Rect>(visible, nv);
                        _updates = static_cast<uint16_t>(_updates & ~bit);
                }
        }
        if (clip.isEmpty()) return;
        Painter painter(*_surface, clip, Span<const Rect>(damage, nd), client.x, client.y, _tools, DrawTimeoutMs);
        if (w.redraw != nullptr) {
                ++_stats.redraws;
                w.redraw(w.arg, painter);
        } else {
                (void)painter.fill(Rect{0, 0, client.width, client.height}, _palette.color(Role::Client));
        }
        count(painter);
        return;
}

void WindowManager::drawUpdates() {
        for (size_t z = 0; z < _stack.count() && _updates != 0; ++z)
                if ((_updates & (1u << (_stack.idAt(z) - 1))) != 0) drawLayer(z, Span<const Rect>());
        _updates = 0; // windows closed since asking
        return;
}

// @p rect painted from the desktop up, each window over the one under it.
void WindowManager::drawBottomUp(const Rect &rect) {
        ++_stats.calls;
        if (_surface->fill(rect, _palette.color(Role::Desktop), DrawTimeoutMs).isError()) ++_stats.errors;
        for (size_t z = _stack.count(); z-- > 0;) {
                const Rect cut = rect.intersected(_stack.rectAt(z));
                if (!cut.isEmpty()) drawLayer(z, Span<const Rect>(&cut, 1));
        }
        return;
}

void WindowManager::drawOutline() {
        Rect sides[4];
        WindowFrame::outline(_outline, sides);
        const Rect screen = _stack.screen();
        Painter    painter(*_surface, Span<const Rect>(&screen, 1), Span<const Rect>(&screen, 1), 0, 0, _tools,
                           DrawTimeoutMs);
        (void)painter.fill(sides, _palette.color(Role::Outline));
        count(painter);
        _outlineShown = true;
        return;
}

void WindowManager::count(const Painter &painter) {
        _stats.calls += painter.calls();
        if (painter.error().isError()) ++_stats.errors;
        return;
}

// After a change: the frames of the windows that lost and gained the focus
// redrawn, the task woken.
void WindowManager::changed(uint8_t oldFocus) {
        const uint8_t top = focusId();
        if (top != oldFocus) {
                _listChanged = true;
                for (const uint8_t id : {oldFocus, top}) {
                        auto [z, err] = _stack.layerOf(id);
                        if (err.isError()) continue;
                        const WindowFrame f = frame(id, _stack.rectAt(z));
                        Rect              edges[4];
                        const size_t      n = f.edges(edges);
                        (void)_stack.invalidate(id, f.titleBar());
                        for (size_t i = 0; i < n; ++i) (void)_stack.invalidate(id, edges[i]);
                }
        }
        _requests.fetch_add(1, std::memory_order_acq_rel);
        _wake.release();
        return;
}

// @p rect on 4-pixel columns, moved wholly onto the screen.
Rect WindowManager::placed(const Rect &rect) const {
        const Rect screen = _stack.screen();
        Rect       r = rect;
        r.width = static_cast<uint16_t>((r.width + Align - 1u) & ~(Align - 1u));
        if (r.width > screen.width) r.width = static_cast<uint16_t>(screen.width & ~(Align - 1u));
        if (r.height > screen.height) r.height = screen.height;
        if (r.right() > screen.width) r.x = static_cast<uint16_t>(screen.width - r.width);
        if (r.bottom() > screen.height) r.y = static_cast<uint16_t>(screen.height - r.height);
        // Its title bar out from under a panel at the top, where it fits.
        if (r.y < _workArea.y && uint32_t(_workArea.y) + r.height <= screen.height) r.y = _workArea.y;
        r.x = static_cast<uint16_t>(r.x & ~(Align - 1u));
        return r;
}

Result<uint8_t> WindowManager::open(const WindowConfig &config) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return makeError<uint8_t>(Error::InvalidState);
        return openLocked(config);
}

Result<uint8_t> WindowManager::openLocked(const WindowConfig &config) {
        // Panels, popups and the desktop's go where asked (on the screen).
        Rect r = config.rect;
        if (WindowStack::isApplication(config.level) && config.cascade) {
                // The next free place down the cascade, among the windows shown.
                Rect   taken[WindowStack::MaxWindows];
                size_t n = 0;
                for (size_t z = 0; z < _stack.count(); ++z)
                        if (WindowStack::isApplication(_stack.levelOf(_stack.idAt(z)))) taken[n++] = _stack.rectAt(z);
                const auto width = static_cast<uint16_t>((r.width + Align - 1u) & ~(Align - 1u));
                r = cascadeRect(width, r.height, _workArea, Span<const Rect>(taken, n));
        }
        if (WindowStack::isApplication(config.level)) {
                r = placed(r);
        } else {
                const Rect screen = _stack.screen();
                r.width = static_cast<uint16_t>((r.width + Align - 1u) & ~(Align - 1u));
                if (r.width > screen.width) r.width = static_cast<uint16_t>(screen.width & ~(Align - 1u));
                if (r.height > screen.height) r.height = screen.height;
                if (r.right() > screen.width) r.x = static_cast<uint16_t>(screen.width - r.width);
                if (r.bottom() > screen.height) r.y = static_cast<uint16_t>(screen.height - r.height);
                r.x = static_cast<uint16_t>(r.x & ~(Align - 1u));
        }
        if (!WindowFrame::isValid(config.decorations) || r.width < WindowFrame::minWidth(config.decorations) ||
            r.height < WindowFrame::minHeight(config.decorations) || config.level > Level::Popup)
                return makeError<uint8_t>(Error::Invalid);
        const uint8_t oldTop = focusId();
        auto [id, err] = _stack.open(r, config.level);
        if (err.isError()) return makeError<uint8_t>(err);
        Window &w = _windows[id - 1];
        w = Window();
        if (config.title != nullptr) copyTitle(w.title, StringView(config.title));
        w.redraw = config.redraw;
        w.closed = config.closed;
        w.key = config.key;
        w.resized = config.resized;
        w.arg = config.arg;
        w.decorations = config.decorations;
        w.widthStep = config.widthStep > 0 ? config.widthStep : 1;
        w.heightStep = config.heightStep > 0 ? config.heightStep : 1;
        w.pointer = config.pointer;
        w.hideWhileTyping = config.hidePointerWhileTyping;
        w.wheel = config.wheel;
        w.client = config.client < MaxClients + 1 ? config.client : 0;
        w.dump = config.dump;
        w.reference = config.reference;
        if (config.kind != nullptr) {
                std::strncpy(w.kind, config.kind, MaxKind);
                w.kind[MaxKind] = '\0';
        }
        w.mouse = config.mouse;
        w.reserve = config.level == Level::Panel ? config.reserve : 0;
        w.focusable = config.focusable && WindowStack::isApplication(config.level);
        if (w.focusable) _focusId = id;
        if (WindowStack::isApplication(config.level)) _listChanged = true;
        if (w.reserve != 0) workAreaChanged();
        if (config.level == Level::Popup && _mouseId != 0 && _mouseId != id) {
                // A menu opened by a press: the pointer is the menu's now (a drag onto an
                // item, a release there), not the pressed window's, which is told it left.
                const uint8_t was = _mouseId;
                _mouseId = 0;
                _hoverId = 0;
                mouseTo(was, MouseEvent::Type::Leave, _pointerX, _pointerY, 0);
        }
        changed(oldTop);
        return makeResult(id);
}

Error WindowManager::close(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return closeLocked(id);
}

Error WindowManager::closeLocked(uint8_t id) {
        const uint8_t oldTop = focusId();
        const Level   level = _stack.levelOf(id);
        const Error   err = _stack.close(id);
        if (err.isError()) return err;
        const Window w = _windows[id - 1];
        _windows[id - 1] = Window();
        if (w.closed != nullptr) w.closed(w.arg);
        if (_grabId == id) _grabId = 0;
        if (_sizeId == id) _sizeId = 0;
        if (_pressId == id) _pressId = 0;
        if (_clickId == id) _clickId = 0;
        if (_mouseId == id) _mouseId = 0;
        if (_hoverId == id) _hoverId = 0;
        if (WindowStack::isApplication(level)) _listChanged = true;
        if (_menuFor == id && _menuId != 0) (void)closeLocked(_menuId); // its menu with it
        if (_switcher != nullptr && id != _switchId && WindowStack::isApplication(level))
                switchEnd(false); // the list is out of date
        if (w.reserve != 0) workAreaChanged();
        if (_dragId == id) {
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
                _dragId = 0;
                _outlineShown = false;
        }
        changed(oldTop);
        return Error::Ok;
}

Error WindowManager::move(uint8_t id, uint16_t x, uint16_t y) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        if (_windows[id - 1].maximized) return Error::InvalidState;
        err = _stack.move(id, placed(Rect{x, y, at.width, at.height}));
        if (err.isOk() && _windows[id - 1].reserve != 0) workAreaChanged();
        if (err.isOk()) changed(focusId());
        return err;
}

Error WindowManager::resize(uint8_t id, uint16_t width, uint16_t height) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        const Window &w = _windows[id - 1];
        if (w.maximized) return Error::InvalidState;
        const Rect to = placed(Rect{at.x, at.y, width, height});
        if (to.width < WindowFrame::minWidth(w.decorations) || to.height < WindowFrame::minHeight(w.decorations))
                return Error::Invalid;
        return resizeLocked(id, to, w.decorations);
}

Error WindowManager::raise(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return raiseLocked(id);
}

Error WindowManager::raiseLocked(uint8_t id) {
        const uint8_t oldTop = focusId();
        const bool    shown = _stack.isHidden(id);
        const Error   err = shown ? _stack.show(id) : _stack.raise(id);
        if (err.isError()) return err;
        if (_windows[id - 1].focusable) _focusId = id;
        if (shown && WindowStack::isApplication(_stack.levelOf(id))) _listChanged = true;
        if (shown && _windows[id - 1].reserve != 0) workAreaChanged();
        changed(oldTop);
        return err;
}

Error WindowManager::minimize(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return minimizeLocked(id);
}

Error WindowManager::minimizeLocked(uint8_t id) {
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        if (!WindowStack::isApplication(_stack.levelOf(id))) return Error::InvalidState; // panels and menus stay
        if (_stack.isHidden(id)) return Error::Ok;
        const uint8_t oldTop = focusId();
        const Error   err = _stack.hide(id);
        if (err.isError()) return err;
        if (WindowStack::isApplication(_stack.levelOf(id))) _listChanged = true;
        if (_mouseId == id) _mouseId = 0;
        if (_hoverId == id) _hoverId = 0;
        if (_windows[id - 1].reserve != 0) workAreaChanged();
        if (_grabId == id) _grabId = 0;
        if (_sizeId == id) _sizeId = 0;
        if (_pressId == id) _pressId = 0;
        if (_dragId == id) {
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
                _dragId = 0;
                _outlineShown = false;
        }
        changed(oldTop);
        return Error::Ok;
}

Error WindowManager::maximize(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return maximizeLocked(id);
}

Error WindowManager::maximizeLocked(uint8_t id) {
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        if (!WindowStack::isApplication(_stack.levelOf(id))) return Error::InvalidState;
        err = raiseLocked(id);
        if (err.isError()) return err;
        Window &w = _windows[id - 1];
        if (w.maximized) return Error::Ok;
        if (_dragId == id) (void)dropLocked();
        if (_grabId == id) _grabId = 0;
        if (_sizeId == id) _sizeId = 0;
        auto [was, rerr] = _stack.rectOf(id); // where the drop put it
        w.restore = rerr.isOk() ? was : at;
        w.maximized = true;
        _listChanged = true;
        return resizeLocked(id, placed(_workArea), w.decorations);
}

Error WindowManager::restore(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return restoreLocked(id);
}

Error WindowManager::restoreLocked(uint8_t id) {
        if (_stack.isHidden(id)) return raiseLocked(id);
        if (_stack.layerOf(id).isError()) return Error::NotFound;
        Window &w = _windows[id - 1];
        if (!w.maximized) return Error::Ok;
        w.maximized = false;
        _listChanged = true;
        return resizeLocked(id, placed(w.restore), w.decorations);
}

Result<uint8_t> WindowManager::connect(Gone gone, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return makeError<uint8_t>(Error::InvalidState); // no window system
        for (size_t i = 0; i < MaxClients; ++i) {
                if (_clients[i].used) continue;
                _clients[i] = Client{gone, arg, true};
                return makeResult(static_cast<uint8_t>(i + 1));
        }
        return makeError<uint8_t>(Error::Full);
}

void WindowManager::disconnect(uint8_t client) {
        Mutex::Locker lock(_lock, WaitForever);
        if (client == 0 || client > MaxClients || !_clients[client - 1].used) return;
        _clients[client - 1] = Client();
        // Its windows, shown or hidden, closed.
        for (uint8_t id = 1; id <= WindowStack::MaxWindows; ++id)
                if (_windows[id - 1].client == client && _stack.rectOf(id).isOk()) (void)closeLocked(id);
        return;
}

Error WindowManager::watch(uint8_t client, Watch fn, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (client == 0 || client > MaxClients || !_clients[client - 1].used) return Error::NotFound;
        _clients[client - 1].watch = fn;
        _clients[client - 1].watchArg = arg;
        return Error::Ok;
}

// The focus: the focusable window raised last (an always-on-top window above it doesn't
// take it), or, when that one went, the top focusable window.
uint8_t WindowManager::focusId() const {
        if (_focusId != 0 && _windows[_focusId - 1].focusable && _stack.layerOf(_focusId).isOk()) return _focusId;
        for (size_t z = 0; z < _stack.count(); ++z) {
                const uint8_t id = _stack.idAt(z);
                if (_windows[id - 1].focusable) return id;
        }
        return 0;
}

uint8_t WindowManager::topPopup() const {
        return _stack.count() > 0 && _stack.levelOf(_stack.idAt(0)) == Level::Popup ? _stack.idAt(0) : 0;
}

void WindowManager::closePopups() {
        for (uint8_t id = topPopup(); id != 0; id = topPopup()) (void)closeLocked(id);
        return;
}

// Window @p id's mouse callback, (@p x, @p y) made its client area's.
void WindowManager::mouseTo(uint8_t id, MouseEvent::Type type, uint16_t x, uint16_t y, uint8_t button) {
        const Window &w = _windows[id - 1];
        if (w.mouse == nullptr) return;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return;
        const Rect client = frame(id, at).client();
        MouseEvent event;
        event.type = type;
        event.x = static_cast<int16_t>(int(x) - client.x);
        event.y = static_cast<int16_t>(int(y) - client.y);
        event.button = button;
        event.buttons = _buttons;
        w.mouse(w.arg, event);
        return;
}

// The pointer at (@p x, @p y) with no press held by a client: motion to the
// client area under it, and a leave to the one it left.
void WindowManager::hoverAt(uint16_t x, uint16_t y) {
        const uint8_t id = windowAt(x, y);
        uint8_t       target = 0;
        if (id != 0 && _windows[id - 1].mouse != nullptr) {
                auto [z, err] = _stack.layerOf(id);
                if (err.isOk() && frame(id, _stack.rectAt(z)).partAt(x, y) == WindowFrame::Part::Client) target = id;
        }
        if (_hoverId != 0 && _hoverId != target) mouseTo(_hoverId, MouseEvent::Type::Leave, x, y, 0);
        _hoverId = target;
        if (target != 0) mouseTo(target, MouseEvent::Type::Motion, x, y, 0);
        return;
}

Rect WindowManager::workAreaLocked() const {
        const Rect screen = _stack.screen();
        uint32_t   top = 0;
        uint32_t   bottom = 0;
        for (size_t z = 0; z < _stack.count(); ++z) {
                const uint8_t id = _stack.idAt(z);
                const Window &w = _windows[id - 1];
                if (w.reserve == 0) continue;
                const Rect r = _stack.rectAt(z);
                if (r.y == 0 && w.reserve > top) top = w.reserve;
                if (r.bottom() == screen.height && r.y != 0 && w.reserve > bottom) bottom = w.reserve;
        }
        if (top + bottom >= screen.height) return screen;
        return Rect{screen.x, static_cast<uint16_t>(top), screen.width,
                    static_cast<uint16_t>(screen.height - top - bottom)};
}

// The panels changed: the work area again, and maximized windows fitted to it.
void WindowManager::workAreaChanged() {
        const Rect area = workAreaLocked();
        if (area == _workArea) return;
        _workArea = area;
        for (uint8_t id = 1; id <= WindowStack::MaxWindows; ++id) {
                const Window &w = _windows[id - 1];
                if (w.maximized && _stack.rectOf(id).isOk()) (void)resizeLocked(id, placed(area), w.decorations);
        }
        return;
}

Rect WindowManager::workArea() {
        Mutex::Locker lock(_lock, WaitForever);
        return _workArea;
}

Rect WindowManager::screen() {
        Mutex::Locker lock(_lock, WaitForever);
        return _surface != nullptr ? _stack.screen() : Rect();
}

Error WindowManager::showWindowMenu(uint8_t id, uint16_t x, uint16_t y) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return showWindowMenuLocked(id, Rect{x, y, 0, 0});
}

// Window @p id's menu, placed for @p anchor (a point, or the menu button).
Error WindowManager::showWindowMenuLocked(uint8_t id, const Rect &anchor) {
        if (id == 0 || id > WindowStack::MaxWindows || _stack.rectOf(id).isError()) return Error::NotFound;
        if (_menuId != 0) (void)closeLocked(_menuId);
        const Window &w = _windows[id - 1];
        const bool    hidden = _stack.isHidden(id);
        const auto    off = [](bool enabled) {
                return enabled ? uint8_t(0) : uint8_t(MenuLayout::Disabled);
        };
        const bool app = WindowStack::isApplication(_stack.levelOf(id));
        const bool above = _stack.levelOf(id) == Level::Above;
        _menuItems[0] = {"Restore", "", off(hidden || w.maximized)};
        _menuItems[1] = {"Minimize", "", off(!hidden && (w.decorations & WindowFrame::MinimizeButton) != 0)};
        _menuItems[2] = {"Maximize", "", off(!w.maximized && (w.decorations & WindowFrame::MaximizeButton) != 0)};
        _menuItems[3] = {"", "", MenuLayout::Separator};
        _menuItems[4] = {"Always on Top", "", static_cast<uint8_t>(off(app) | (above ? MenuLayout::Checked : 0))};
        _menuItems[5] = {"", "", MenuLayout::Separator};
        _menuItems[6] = {"Close", "Alt+F4", off((w.decorations & WindowFrame::CloseButton) != 0)};
        constexpr MenuChoice choices[MenuItems] = {MenuChoice::Restore, MenuChoice::Minimize, MenuChoice::Maximize,
                                                   MenuChoice::None,    MenuChoice::Above,    MenuChoice::None,
                                                   MenuChoice::Close};
        for (size_t i = 0; i < MenuItems; ++i) _menuChoices[i] = choices[i];
        Error err = _menuLayout.layout(Span<const MenuLayout::Item>(_menuItems, MenuItems), Fonts::unscii16);
        if (err.isError()) return err;
        WindowConfig config;
        config.rect = MenuLayout::place(_menuLayout.width(), _menuLayout.height(), anchor, _stack.screen(), false);
        config.title = "Window menu";
        config.redraw = menuRedraw;
        config.mouse = menuMouse;
        config.key = menuKey;
        config.closed = menuClosed;
        config.arg = this;
        config.decorations = WindowFrame::None;
        config.level = Level::Popup;
        config.focusable = false;
        config.kind = "menu";
        auto [menu, oerr] = openLocked(config);
        if (oerr.isError()) return oerr;
        _menuId = menu;
        _menuFor = id;
        _menuHighlight = -1;
        return Error::Ok;
}

void WindowManager::menuRedraw(void *arg, Painter &painter) {
        auto *self = static_cast<WindowManager *>(arg);
        drawMenu(painter, self->_menuLayout, Span<const MenuLayout::Item>(self->_menuItems, MenuItems),
                 self->_menuHighlight);
        return;
}

// Item @p item highlighted (-1: none): the two rows redrawn.
void WindowManager::menuHighlight(int item) {
        if (item == _menuHighlight || _menuId == 0) return;
        auto [at, err] = _stack.rectOf(_menuId);
        if (err.isError()) return;
        for (const int i : {_menuHighlight, item}) {
                if (i < 0) continue;
                const Rect r = _menuLayout.itemRect(size_t(i));
                (void)_stack.invalidate(_menuId, Rect{uint16_t(at.x + r.x), uint16_t(at.y + r.y), r.width, r.height});
        }
        _menuHighlight = item;
        changed(focusId());
        return;
}

// Item @p item chosen: the menu closed, then done to its window.
void WindowManager::menuChoose(int item) {
        const uint8_t id = _menuFor;
        if (_menuId != 0) (void)closeLocked(_menuId);
        if (id == 0 || _stack.rectOf(id).isError()) return;
        const MenuChoice choice = item >= 0 && size_t(item) < MenuItems ? _menuChoices[item] : MenuChoice::None;
        switch (choice) {
                case MenuChoice::Restore: (void)restoreLocked(id); break;
                case MenuChoice::Minimize: (void)minimizeLocked(id); break;
                case MenuChoice::Maximize: (void)maximizeLocked(id); break;
                case MenuChoice::Above: {
                        const uint8_t oldFocus = focusId();
                        const bool    above = _stack.levelOf(id) == Level::Above;
                        if (_stack.setLevel(id, above ? Level::Normal : Level::Above).isOk()) {
                                _listChanged = true;
                                changed(oldFocus);
                        }
                        break;
                }
                case MenuChoice::Close: (void)closeLocked(id); break;
                case MenuChoice::None: break;
        }
        return;
}

void WindowManager::menuMouse(void *arg, const MouseEvent &event) {
        auto     *self = static_cast<WindowManager *>(arg);
        const int item = self->_menuLayout.itemAt(event.x, event.y);
        const int selectable = item >= 0 && self->_menuLayout.isSelectable(size_t(item)) ? item : -1;
        switch (event.type) {
                case MouseEvent::Type::Press:
                case MouseEvent::Type::Motion: self->menuHighlight(selectable); break;
                case MouseEvent::Type::Leave: self->menuHighlight(-1); break;
                case MouseEvent::Type::Release:
                        if (selectable >= 0) self->menuChoose(selectable);
                        break;
        }
        return;
}

void WindowManager::menuKey(void *arg, const KeyEvent &event) {
        auto *self = static_cast<WindowManager *>(arg);
        if (event.action == KeyEvent::Action::Release) return;
        if (event.usage == HidKey::Down || event.usage == HidKey::Up) {
                self->menuHighlight(self->_menuLayout.next(self->_menuHighlight, event.usage == HidKey::Down ? 1 : -1));
        } else if (event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter) {
                if (self->_menuHighlight >= 0) self->menuChoose(self->_menuHighlight);
        } else if (event.usage == HidKey::Escape) {
                if (self->_menuId != 0) (void)self->closeLocked(self->_menuId);
        }
        return;
}

void WindowManager::menuClosed(void *arg) {
        auto *self = static_cast<WindowManager *>(arg);
        self->_menuId = 0;
        self->_menuFor = 0;
        self->_menuHighlight = -1;
        return;
}

Error WindowManager::setAbove(uint8_t id, bool above) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        if (!WindowStack::isApplication(_stack.levelOf(id))) return Error::InvalidState;
        const uint8_t oldFocus = focusId();
        const Error   err = _stack.setLevel(id, above ? Level::Above : Level::Normal);
        if (err.isOk()) {
                _listChanged = true;
                changed(oldFocus);
        }
        return err;
}

// ---- Keys the window manager keeps (devplan/gui.md §4g)

namespace {

        constexpr uint8_t  UsageF4 = HidKey::F1 + 3;
        constexpr uint8_t  UsageLeftAlt = 0xe2;
        constexpr uint8_t  UsageRightAlt = 0xe6;
        constexpr uint16_t SwitcherWidth = 320; // a box in the screen's middle, not a menu

} // namespace

// Alt+Tab's list: the windows in it, their texts as shown, which one is chosen.
struct WindowManager::Switcher {
                MenuLayout       layout;
                MenuLayout::Item items[WindowStack::MaxWindows];
                char             texts[WindowStack::MaxWindows][MaxTitle + 3];
                uint8_t          ids[WindowStack::MaxWindows];
                size_t           count = 0;
                int              at = 0;
};

// @p event, if it is one of the window manager's keys: done here, and not delivered.
bool WindowManager::shortcut(const KeyEvent &event) {
        const bool press = event.action != KeyEvent::Action::Release;
        const bool alt = event.hasAlt();
        const bool altKey = event.usage == UsageLeftAlt || event.usage == UsageRightAlt;
        if (_switcher != nullptr) {
                // While the list shows, every key is its: Tab moves, Escape cancels, Alt's release chooses.
                if (press && event.usage == HidKey::Tab) {
                        switchTo(_switcher->at + (event.hasShift() ? -1 : 1));
                } else if (press && event.usage == HidKey::Escape) {
                        switchEnd(false);
                } else if (!alt || (altKey && !press)) {
                        switchEnd(true);
                }
                return true;
        }
        if (!press || !alt || event.hasCtrl() || (event.modifiers & KeyEvent::Gui) != 0) return false;
        if (event.usage == HidKey::Tab) {
                switchOpen(event.hasShift() ? -1 : 1);
                return true;
        }
        const uint8_t focus = focusId();
        if (event.usage == UsageF4 && focus != 0) {
                if ((_windows[focus - 1].decorations & WindowFrame::CloseButton) != 0) (void)closeLocked(focus);
                return true;
        }
        if (event.usage == HidKey::Space && focus != 0) {
                auto [at, err] = _stack.rectOf(focus);
                if (err.isOk()) {
                        const WindowFrame f = frame(focus, at);
                        const Rect        bar = f.has(WindowFrame::TitleBar) ? f.titleBar() : f.client();
                        (void)showWindowMenuLocked(focus, Rect{bar.x, bar.y, 0, bar.height});
                }
                return true;
        }
        return false;
}

// Alt+Tab: the applications' windows listed in the screen's middle, the focus first, then the
// others top to bottom, then the minimized ones; the next (@p step 1) or the last (-1) chosen.
void WindowManager::switchOpen(int step) {
        closePopups();
        auto *sw = static_cast<Switcher *>(Memory::alloc(MemSpace::Psram, sizeof(Switcher)));
        if (sw == nullptr) return;
        ::new (sw) Switcher();
        // The focus first (Alt+Tab once: the window before it), then the rest top to bottom.
        const size_t  shown = _stack.count();
        const uint8_t focus = focusId();
        for (size_t k = 0; k <= shown + _stack.hiddenCount(); ++k) {
                const size_t  i = k == 0 ? 0 : k - 1;
                const uint8_t id = k == 0 ? focus : i < shown ? _stack.idAt(i) : _stack.hiddenIdAt(i - shown);
                if (id == 0 || (k > 0 && id == focus) || !WindowStack::isApplication(_stack.levelOf(id))) continue;
                const char *title = _windows[id - 1].title[0] != '\0' ? _windows[id - 1].title : "(untitled)";
                char       *text = sw->texts[sw->count];
                std::snprintf(text, sizeof(sw->texts[0]), _stack.isHidden(id) ? "[%s]" : "%s", title);
                sw->items[sw->count] = MenuLayout::Item{StringView(text), StringView(), 0};
                sw->ids[sw->count++] = id;
        }
        if (sw->count == 0 ||
            sw->layout.layout(Span<const MenuLayout::Item>(sw->items, sw->count), Fonts::unscii16, SwitcherWidth)
                    .isError()) {
                Memory::free(sw);
                return;
        }
        sw->at = sw->count == 1 ? 0 : (step > 0 ? 1 : int(sw->count) - 1);
        const Rect     screen = _stack.screen();
        const uint16_t w = sw->layout.width() < screen.width ? sw->layout.width() : screen.width;
        const uint16_t h = sw->layout.height() < screen.height ? sw->layout.height() : screen.height;
        WindowConfig   config;
        config.rect = Rect{static_cast<uint16_t>(((screen.width - w) / 2) & ~(Align - 1u)),
                           static_cast<uint16_t>((screen.height - h) / 2), w, h};
        config.title = "Switch to";
        config.redraw = switchRedraw;
        config.mouse = switchMouse;
        config.closed = switchClosed;
        config.arg = this;
        config.decorations = WindowFrame::None;
        config.level = Level::Popup;
        config.focusable = false;
        config.kind = "switcher";
        _switcher = sw;
        auto [id, err] = openLocked(config);
        if (err.isError()) {
                _switcher = nullptr;
                Memory::free(sw);
                return;
        }
        _switchId = id;
        return;
}

// Row @p row of popup @p id, laid out by @p layout, redrawn.
void WindowManager::invalidateRow(uint8_t id, const MenuLayout &layout, int row) {
        if (row < 0) return;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return;
        const Rect r = layout.itemRect(size_t(row));
        (void)_stack.invalidate(id, Rect{uint16_t(at.x + r.x), uint16_t(at.y + r.y), r.width, r.height});
        return;
}

// The list's choice moved to @p item (around the ends): only the two rows redrawn.
void WindowManager::switchTo(int item) {
        Switcher *sw = _switcher;
        if (sw == nullptr) return;
        const int n = int(sw->count);
        item = ((item % n) + n) % n;
        if (item == sw->at) return;
        invalidateRow(_switchId, sw->layout, sw->at);
        invalidateRow(_switchId, sw->layout, item);
        sw->at = item;
        changed(focusId());
        return;
}

// The list closed; with @p go, its choice raised (shown again if minimized).
void WindowManager::switchEnd(bool go) {
        if (_switcher == nullptr) return;
        const uint8_t id = go ? _switcher->ids[_switcher->at] : 0;
        if (_switchId != 0) (void)closeLocked(_switchId); // frees the list (switchClosed)
        if (id != 0 && _stack.rectOf(id).isOk()) (void)raiseLocked(id);
        return;
}

void WindowManager::switchRedraw(void *arg, Painter &painter) {
        auto *self = static_cast<WindowManager *>(arg);
        if (self->_switcher == nullptr) return;
        const Switcher &sw = *self->_switcher;
        drawMenu(painter, sw.layout, Span<const MenuLayout::Item>(sw.items, sw.count), sw.at);
        return;
}

void WindowManager::switchMouse(void *arg, const MouseEvent &event) {
        auto *self = static_cast<WindowManager *>(arg);
        if (self->_switcher == nullptr) return;
        const int item = self->_switcher->layout.itemAt(event.x, event.y);
        if (item < 0) return;
        if (event.type == MouseEvent::Type::Motion || event.type == MouseEvent::Type::Press) self->switchTo(item);
        if (event.type == MouseEvent::Type::Release) {
                self->switchTo(item);
                self->switchEnd(true);
        }
        return;
}

void WindowManager::switchClosed(void *arg) {
        auto *self = static_cast<WindowManager *>(arg);
        Memory::free(self->_switcher);
        self->_switcher = nullptr;
        self->_switchId = 0;
        return;
}

Error WindowManager::dump(uint8_t id, Stream &out) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (id == 0 || id > WindowStack::MaxWindows || _stack.rectOf(id).isError()) return Error::NotFound;
        const Window &w = _windows[id - 1];
        if (w.dump == nullptr) return Error::NotSupported;
        w.dump(w.arg, out);
        return Error::Ok;
}

Error WindowManager::reference(uint8_t id, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (id == 0 || id > WindowStack::MaxWindows || _stack.rectOf(id).isError()) return Error::NotFound;
        const Window &w = _windows[id - 1];
        if (w.reference == nullptr) return Error::NotSupported;
        w.reference(w.arg, pixels, stride, width, height, _palette);
        return Error::Ok;
}

Error WindowManager::setTitle(uint8_t id, StringView title) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        copyTitle(_windows[id - 1].title, title);
        if (!_stack.isHidden(id)) (void)_stack.invalidate(id, frame(id, at).titleBar());
        if (WindowStack::isApplication(_stack.levelOf(id))) _listChanged = true;
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::setIcon(uint8_t id, const Icon *icon) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        _windows[id - 1].icon = icon;
        if (WindowStack::isApplication(_stack.levelOf(id))) _listChanged = true;
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::setDecorations(uint8_t id, uint8_t decorations) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        if (!WindowFrame::isValid(decorations) || at.width < WindowFrame::minWidth(decorations) ||
            at.height < WindowFrame::minHeight(decorations))
                return Error::Invalid;
        return resizeLocked(id, at, decorations);
}

// Window @p id at @p rect with @p decorations: all of it redrawn, and told
// its client area's size if that changed.
Error WindowManager::resizeLocked(uint8_t id, const Rect &rect, uint8_t decorations) {
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        Window       &w = _windows[id - 1];
        const Rect    before = WindowFrame(at, w.decorations).client();
        const uint8_t oldTop = focusId();
        err = _stack.move(id, rect);
        if (err.isError()) return err;
        w.decorations = decorations;
        (void)_stack.invalidate(id, rect); // the frame changed, or the size: all of it
        const Rect after = WindowFrame(rect, decorations).client();
        if ((after.width != before.width || after.height != before.height) && w.resized != nullptr)
                w.resized(w.arg, after.width, after.height);
        if (w.reserve != 0) workAreaChanged();
        changed(oldTop);
        return Error::Ok;
}

Error WindowManager::invalidate(uint8_t id, const Rect &rect) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        if (_stack.isHidden(id)) return Error::Ok; // drawn when shown
        const Rect client = frame(id, at).client();
        const Rect local{0, 0, client.width, client.height};
        const Rect r = rect.intersected(local);
        if (r.isEmpty()) return Error::Ok;
        (void)_stack.invalidate(id, Rect{uint16_t(client.x + r.x), uint16_t(client.y + r.y), r.width, r.height});
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::update(uint8_t id) {
        if (id == 0 || id > WindowStack::MaxWindows) return Error::NotFound;
        // Without the lock: a writer on another task doesn't wait for a round.
        _asked.fetch_or(static_cast<uint16_t>(1u << (id - 1)), std::memory_order_acq_rel);
        _requests.fetch_add(1, std::memory_order_acq_rel);
        _wake.release();
        return Error::Ok;
}

Error WindowManager::setPalette(const WindowPalette &palette) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        // The desktop alone changed: only it redrawn.
        WindowPalette windows = palette;
        windows.setColor(Role::Desktop, _palette.color(Role::Desktop));
        if (windows == _palette) {
                _stack.invalidateDesktop(_stack.screen());
                // And the desktop's own windows (a panel's), which draw in its color.
                for (size_t z = 0; z < _stack.count(); ++z)
                        if (_stack.levelOf(_stack.idAt(z)) == Level::Desktop)
                                (void)_stack.invalidate(_stack.idAt(z), _stack.rectAt(z));
        } else {
                _stack.invalidateScreen(_stack.screen());
        }
        _palette = palette;
        changed(focusId());
        return Error::Ok;
}

WindowPalette WindowManager::palette() {
        Mutex::Locker lock(_lock, WaitForever);
        return _palette;
}

Error WindowManager::drag(uint8_t id, uint16_t x, uint16_t y) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return dragLocked(id, x, y);
}

Error WindowManager::dragLocked(uint8_t id, uint16_t x, uint16_t y) {
        if (_dragId != 0 && _dragId != id) return Error::Busy;
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return err;
        if (_windows[id - 1].maximized) return Error::InvalidState;
        const Rect at = _stack.rectAt(z);
        return outlineLocked(id, placed(Rect{x, y, at.width, at.height}));
}

// Window @p id's outline shown at @p to (placed): a move or a resize under way.
Error WindowManager::outlineLocked(uint8_t id, const Rect &to) {
        if (_dragId != 0 && _dragId != id) return Error::Busy;
        if (_dragId == id && to == _outline) return Error::Ok;
        if (_outlineShown) {
                // What the outline covered, redrawn by its owners.
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
        }
        _dragId = id;
        _outline = to;
        _outlineShown = false;
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::drop() {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return dropLocked();
}

Error WindowManager::dropLocked() {
        if (_dragId == 0) return Error::InvalidState;
        const uint8_t id = _dragId;
        const uint8_t oldTop = focusId();
        if (_outlineShown) {
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
        }
        _dragId = 0;
        _outlineShown = false;
        Error err = _stack.raise(id);
        if (err.isOk()) err = resizeLocked(id, _outline, _windows[id - 1].decorations); // told if resized
        changed(oldTop);
        return err;
}

size_t WindowManager::list(Span<Info> out) {
        Mutex::Locker lock(_lock, WaitForever);
        size_t        n = 0;
        const size_t  shown = _stack.count();
        for (size_t i = 0; i < shown + _stack.hiddenCount() && n < out.size(); ++i, ++n) {
                Info &info = out[n];
                info.id = i < shown ? _stack.idAt(i) : _stack.hiddenIdAt(i - shown);
                info.rect = _stack.rectOf(info.id).value();
                const Window &w = _windows[info.id - 1];
                std::memcpy(info.title, w.title, sizeof(info.title));
                info.focused = info.id == focusId();
                info.minimized = i >= shown;
                info.maximized = w.maximized;
                info.decorations = w.decorations;
                std::memcpy(info.kind, w.kind, sizeof(info.kind));
                info.level = _stack.levelOf(info.id);
                info.hasIcon = w.icon != nullptr;
        }
        return n;
}

Result<Rect> WindowManager::rect(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        return _stack.rectOf(id);
}

Result<Rect> WindowManager::clientRect(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return makeError<Rect>(err);
        return makeResult(frame(id, at).client());
}

Error WindowManager::sync(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        uint32_t       target = 0;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (_surface == nullptr) return Error::InvalidState;
                target = _requests.load(std::memory_order_acquire);
        }
        auto drawn = [this, target] {
                return static_cast<int32_t>(_drawn.load(std::memory_order_acquire) - target) >= 0;
        };
        while (!drawn()) {
                Semaphore       woken(1, 0);
                WaitList::Entry entry(_idle, woken);
                if (!entry.isAdded()) return Error::Busy;
                if (drawn()) break;
                if (woken.acquire(deadline).isError() && !drawn()) return Error::Timeout;
        }
        Surface *surface = nullptr;
        uint32_t presented = 0;
        {
                Mutex::Locker lock(_lock, WaitForever);
                surface = _surface;
                presented = _presented;
        }
        if (surface == nullptr || surface->buffering() == Surface::Buffering::Single) return Error::Ok;
        const int64_t us = deadline.remainingUs();
        return surface->waitShown(presented, us == Deadline::Forever ? WaitForever : unsigned(us / 1000 + 1));
}

WindowManager::Stats WindowManager::stats() {
        Mutex::Locker lock(_lock, WaitForever);
        return _stats;
}

BMDS_NAMESPACE_END
