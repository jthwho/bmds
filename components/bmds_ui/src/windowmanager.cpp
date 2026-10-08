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
#include <algorithm>
#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/deadline.h>
#include <bmds/display.h>
#include <bmds/font.h>
#include <bmds/input.h>
#include <bmds/memory.h>
#include <bmds/mnemonic.h>
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
        _under = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, UnderBytes));
        Error err = _tools.strip == nullptr || _pieces == nullptr || _scratch == nullptr || _under == nullptr
                            ? Error(Error::NoMem)
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
                Memory::free(_under);
                _tools = Painter::Tools();
                _pieces = nullptr;
                _scratch = nullptr;
                _under = nullptr;
                return err;
        }
        _tools.halfWidth = half;
        _tools.height = Fonts::unscii16.height();
        _tools.renderer = &_renderer;
        _tools.palette = &_palette;
        _tools.busyHalf = -1;
        for (uint32_t &word : _updates) word = 0;
        _display = &display;
        _surface = &surface;
        _palette = palette;
        _stack.reset(surface.width(), surface.height()); // all damaged: the desktop drawn
        _windows.clear();
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
        _underRect = Rect();
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
        _menuCount = 0;
        _desktopCount = 0; // the starter sets the menu (setDesktopMenu())
        _desktopChosen = nullptr;
        _desktopArg = nullptr;
        _desktopPicked = -1;
        _programChosen = nullptr;
        _programArg = nullptr;
        _programPicked = -1;
        _canMinimize = false;
        _menuHighlight = -1;
        _focusId = 0;
        for (KeyGrab &g : _grabs) g = KeyGrab();
        _tapUsage = 0;
        _tapClean = false;
        _snap = Snap::None;
        _unsnapId = 0;
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
        for (Client &c : _clients)
                if (c.used && c.gone != nullptr) c.gone(c.arg);
        _clients.clear();
        dropServices(0);
        _windows.clear();
        (void)_surface->wait(DrawTimeoutMs); // the strip's last copy
        _surface = nullptr;
        _display->release();
        _display = nullptr;
        Memory::free(_tools.strip);
        Memory::free(_pieces);
        Memory::free(_scratch);
        Memory::free(_under);
        _tools = Painter::Tools();
        _pieces = nullptr;
        _scratch = nullptr;
        _under = nullptr;
        _underRect = Rect();
        _stack.reset(0, 0);
        _dragId = 0;
        for (std::atomic<uint32_t> &word : _asked) word.store(0, std::memory_order_release);
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
                Surface      *surface = nullptr;
                uint32_t      frame = 0;
                int           picked = -1;
                DesktopChosen chosen = nullptr;
                void         *chosenArg = nullptr;
                int           programPicked = -1;
                MenuChosen    programFn = nullptr;
                void         *programArg = nullptr;
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
                                picked = _desktopPicked;
                                _desktopPicked = -1;
                                chosen = _desktopChosen;
                                chosenArg = _desktopArg;
                                programPicked = _programPicked;
                                _programPicked = -1;
                                programFn = _pickedFn;
                                programArg = _pickedArg;
                        }
                }
                // A desktop menu's choice, told without the lock (it may start a program), and a
                // program's menu's.
                if (picked >= 0 && chosen != nullptr) chosen(chosenArg, size_t(picked));
                if (programPicked >= 0 && programFn != nullptr) programFn(programArg, size_t(programPicked));
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
                // The modifiers down after this event (a press and Alt+drag use them). A modifier's
                // own release carries the modifiers as they were (the decoder's), its bit still set:
                // taken off here, or Alt would stay down until the next key.
                _modifiers = event.modifiers;
                if (event.usage >= 0xe0 && event.usage <= 0xe7) {
                        const auto bit = static_cast<uint8_t>(1u << (event.usage - 0xe0));
                        _modifiers = static_cast<uint8_t>(event.action == KeyEvent::Action::Release ? _modifiers & ~bit
                                                                                                    : _modifiers | bit);
                }
                // The window manager's own keys first (not one sent to a chosen window: a test's).
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
                        if (isCursorDrawn()) _cursorMoved = true; // what it covered put back
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
                const bool wasDrawn = isCursorDrawn();
                const bool moved = nx != _pointerX || ny != _pointerY;
                if (moved || event.buttons != _buttons || event.wheel != 0) _typingHidden = false; // the mouse is back
                if (moved) {
                        _pointerX = nx;
                        _pointerY = ny;
                        if (_grabId != 0 && _unsnapId == _grabId) {
                                // A snapped window dragged away: its size back, under the pointer.
                                Window    &w = _windows[_grabId - 1];
                                const int  bx = int(nx) - _grabX, by = int(ny) - _grabY;
                                const Rect back{static_cast<uint16_t>(bx < 0 ? 0 : bx),
                                                static_cast<uint16_t>(by < 0 ? 0 : by), w.restore.width,
                                                w.restore.height};
                                w.snapped = false;
                                _unsnapId = 0;
                                _listChanged = true;
                                (void)resizeLocked(_grabId, placed(back), w.decorations);
                        }
                        if (_grabId != 0) {
                                const Snap snap = snapAt(_grabId, nx, ny);
                                if (snap != Snap::None) {
                                        // The outline where the window would snap to.
                                        _snap = snap;
                                        (void)outlineLocked(_grabId, snapRect(snap));
                                } else {
                                        _snap = Snap::None;
                                        const int wx = int(nx) - _grabX, wy = int(ny) - _grabY;
                                        (void)dragLocked(_grabId, static_cast<uint16_t>(wx < 0 ? 0 : wx),
                                                         static_cast<uint16_t>(wy < 0 ? 0 : wy));
                                }
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
                        // To the window under the pointer, focused or not; over its title bar, it rolls
                        // the window up (away from the user) or down.
                        const uint8_t id = windowAt(nx, ny);
                        const Window *w = id != 0 ? &_windows[id - 1] : nullptr;
                        auto [z, zerr] = _stack.layerOf(id);
                        const bool onTitle = w != nullptr && zerr.isOk() &&
                                             frame(id, _stack.rectAt(z)).partAt(nx, ny) == WindowFrame::Part::TitleBar;
                        if (onTitle) {
                                (void)shadeLocked(id, event.wheel > 0);
                        } else if (w != nullptr && w->wheel != nullptr) {
                                w->wheel(w->arg, event.wheel);
                        }
                }
                const Pointer shape = shapeAt(nx, ny);
                const bool    changed = moved || shape != _shape;
                _shape = shape;
                _cursorShown = true;
                const bool drawn = isCursorDrawn();
                // What it covered is put back from what was saved (restoreUnder()): no window redraws.
                if ((wasDrawn && (changed || !drawn)) || (drawn && (changed || !wasDrawn))) _cursorMoved = true;
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
        if (id == 0) {
                // The bare desktop: the window manager's own menu (no program's desktop window here).
                if (button == PointerEvent::Right) (void)showDesktopMenuLocked(x, y);
                return;
        }
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return;
        const WindowFrame f = frame(id, _stack.rectAt(z));
        const Part        part = f.partAt(x, y);
        (void)raiseLocked(id, _windows[id - 1].focusOnPress);
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
                if (_windows[id - 1].maximized || _windows[id - 1].shaded)
                        return; // sized with the screen, or rolled up
                _sizeId = id;
                _sizeEdges = edges;
                _sizeStart = f.outer;
                _sizeX = x;
                _sizeY = y;
                return;
        }
        if (part == Part::Minimize && !_canMinimize) return; // nowhere to go: disabled
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
        Window &w = _windows[id - 1];
        if (w.maximized) return; // stays where it is
        _grabId = id;
        _grabX = static_cast<uint16_t>(x - f.outer.x);
        _grabY = static_cast<uint16_t>(y - f.outer.y);
        _snap = Snap::None;
        if (w.snapped) {
                // Its own size back when it moves: the same place across its title bar under the pointer.
                _unsnapId = id;
                _grabX =
                        static_cast<uint16_t>(uint32_t(_grabX) * w.restore.width / (f.outer.width ? f.outer.width : 1));
        }
        return;
}

void WindowManager::release(uint16_t x, uint16_t y) {
        if (_grabId != 0) {
                if (_dragId == _grabId && _snap != Snap::None) {
                        const uint8_t id = _grabId;
                        hideOutline();
                        (void)snapLocked(id, _snap); // maximized, or half the work area
                } else if (_dragId == _grabId) {
                        (void)dropLocked(); // moved: put there
                }
                _grabId = 0;
                _unsnapId = 0;
                _snap = Snap::None;
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
        _pointerHidden = !visible;
        if (wasDrawn || isCursorDrawn()) _cursorMoved = true;
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
                        uint8_t       *under; // what the area holds, saved there first
        };

        // What the pointer covers: its area's bytes, a row after another.
        struct UnderJob {
                        uint8_t *under;
                        uint16_t width;
                        uint16_t rows;
        };

        // UnderBytes holds the pointer's largest area (a picture widened to 4-pixel columns).
        static_assert(Surface::BytesPerPixel == 3);
        static_assert([] {
                for (const Picture &p : Pictures)
                        if ((p.width + 2u * 4u) * p.height * Surface::BytesPerPixel > WindowManager::UnderBytes)
                                return false;
                return true;
        }());

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
        const size_t   bytes = size_t(job->width) * Surface::BytesPerPixel;
        for (uint16_t r = 0; r < job->rows; ++r)
                std::memcpy(job->under + r * bytes, pixels + size_t(r) * stride, bytes);
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

// What @p arg (the saved pixels) holds, back into @p pixels (the area they came from).
void WindowManager::restorePixels(void *arg, uint8_t *pixels, size_t stride) {
        const auto  *job = static_cast<const UnderJob *>(arg);
        const size_t row = size_t(job->width) * Surface::BytesPerPixel;
        for (uint16_t r = 0; r < job->rows; ++r) std::memcpy(pixels + size_t(r) * stride, job->under + r * row, row);
        return;
}

// What the pointer covered put back, before anything is drawn this round: so a move asks no window
// to redraw, and nothing drawn copies the pointer's picture (a scroll).
void WindowManager::restoreUnder() {
        if (_underRect.isEmpty()) return;
        UnderJob job{_under, _underRect.width, _underRect.height};
        ++_stats.calls;
        if (_surface->patch(_underRect, restorePixels, &job, DrawTimeoutMs).isError()) ++_stats.errors;
        _underRect = Rect();
        return;
}

// The pointer over everything drawn this round, what it covers saved first (restoreUnder()).
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
        job.under = _under;
        ++_stats.calls;
        if (_surface->patch(area, drawCursorPixels, &job, DrawTimeoutMs).isError()) {
                ++_stats.errors;
                return;
        }
        _underRect = area;
        return;
}

// Everything damaged drawn by whoever shows it, the outline over it; then
// (double-buffered) presented. With the lock held.
// The changes counted up to @p requests are drawn by it.
void WindowManager::round(uint32_t requests) {
        for (size_t i = 0; i < UpdateWords; ++i) // after the count
                _updates[i] |= _asked[i].exchange(0, std::memory_order_acq_rel);
        const Region damage = _stack.damage();
        _stack.clearDamage();
        const bool drawing =
                !damage.isEmpty() || hasUpdates() || (_dragId != 0 && !_outlineShown) || _cursorMoved || _bandMoved;
        if (!drawing) {
                _drawn.store(requests, std::memory_order_release);
                return;
        }
        const uint64_t start = System::uptimeUs();
        restoreUnder();
        restoreBand(); // under the pointer's: put back in the order they went on
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
        if (_bandId != 0) drawBand(); // over what was just drawn under it
        _bandMoved = false;
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
        const uint32_t    bit = 1u << (id % 32);
        uint32_t         &word = _updates[id / 32];
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
                        {Part::Menu, Role::Button, Role::ButtonMark, bmds::IconGlyph::Menu},
                        {Part::Minimize, Role::Button, Role::ButtonMark, bmds::IconGlyph::Minimize},
                        {Part::Maximize, Role::Button, Role::ButtonMark,
                         w.maximized ? bmds::IconGlyph::Restore : bmds::IconGlyph::Maximize},
                        {Part::Close, Role::CloseBox, Role::CloseMark, bmds::IconGlyph::Close},
                };
                for (const Button &b : buttons) {
                        const Rect r = f.button(b.part);
                        if (r.isEmpty()) continue;
                        if (b.part == Part::Menu && w.icon != nullptr && w.icon->isValid() &&
                            w.icon->width <= r.width && w.icon->height <= r.height) {
                                // The window's icon in its place, on the title bar (it opens the menu as well).
                                (void)chrome.image(r.x, r.y, *w.icon, color);
                                continue;
                        }
                        const bool pressed = _pressId == id && _pressPart == b.part && _pressShown;
                        TextCell   icon;
                        icon.codepoint = b.icon;
                        icon.fg =
                                _palette.color(b.part == Part::Minimize && !_canMinimize ? Role::DisabledText : b.mark);
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
        const bool       update = (word & bit) != 0;
        if (update) {
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
                        word &= ~bit;
                }
        }
        if (clip.isEmpty()) return;
        Painter painter(*_surface, clip, Span<const Rect>(damage, nd), client.x, client.y, _tools, DrawTimeoutMs);
        if (w.redraw != nullptr) {
                ++_stats.redraws;
                w.redraw(w.arg, painter);
                if (painter.isDeferred()) {
                        // Its owner was busy: the same asked again next round (a frame later).
                        ++_stats.deferred;
                        if (update) _asked[id / 32].fetch_or(bit, std::memory_order_acq_rel);
                        for (size_t i = 0; i < nd; ++i) (void)_stack.invalidate(id, damage[i]);
                        _requests.fetch_add(1, std::memory_order_acq_rel);
                        _wake.release();
                }
        } else {
                (void)painter.fill(Rect{0, 0, client.width, client.height}, _palette.color(Role::Client));
        }
        count(painter);
        return;
}

bool WindowManager::hasUpdates() const {
        uint32_t any = 0;
        for (const uint32_t word : _updates) any |= word;
        return any != 0;
}

void WindowManager::drawUpdates() {
        for (size_t z = 0; z < _stack.count() && hasUpdates(); ++z) {
                const uint8_t id = _stack.idAt(z);
                if ((_updates[id / 32] & (1u << (id % 32))) != 0) drawLayer(z, Span<const Rect>());
        }
        for (uint32_t &word : _updates) word = 0; // windows closed since asking
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

// The outline: a light band between two dark lines, seen on any background (inverting what is
// under it would read the screen back, and shows nothing on a middle grey).
void WindowManager::drawOutline() {
        Rect sides[4];
        WindowFrame::outline(_outline, sides);
        const Rect screen = _stack.screen();
        Painter    painter(*_surface, Span<const Rect>(&screen, 1), Span<const Rect>(&screen, 1), 0, 0, _tools,
                           DrawTimeoutMs);
        (void)painter.fill(sides, _palette.color(Role::Outline));
        const uint16_t t = WindowFrame::OutlineWidth;
        const Rect    &o = _outline;
        if (o.width > 2 * t && o.height > 2 * t) {
                // A pixel wide at the outer edge and at the inner one.
                const Rect inner{uint16_t(o.x + t - 1), uint16_t(o.y + t - 1), uint16_t(o.width - 2 * (t - 1)),
                                 uint16_t(o.height - 2 * (t - 1))};
                Rect       lines[8];
                size_t     n = 0;
                for (const Rect &r : {o, inner}) {
                        lines[n++] = Rect{r.x, r.y, r.width, 1};
                        lines[n++] = Rect{r.x, uint16_t(r.bottom() - 1), r.width, 1};
                        lines[n++] = Rect{r.x, uint16_t(r.y + 1), 1, uint16_t(r.height - 2)};
                        lines[n++] = Rect{uint16_t(r.right() - 1), uint16_t(r.y + 1), 1, uint16_t(r.height - 2)};
                }
                (void)painter.fill(Span<const Rect>(lines, n), _palette.color(Role::FaceDark));
        }
        count(painter);
        _outlineShown = true;
        return;
}

Error WindowManager::setBand(uint8_t id, const Rect &rect) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        const Rect client = frame(id, at).client();
        const Rect local = rect.intersected(Rect{0, 0, client.width, client.height});
        const Rect screen = local.isEmpty() ? Rect()
                                            : Rect{uint16_t(client.x + local.x), uint16_t(client.y + local.y),
                                                   local.width, local.height};
        if (screen.isEmpty() && _bandId == 0) return Error::Ok;
        if (_bandId == id && screen == _band) return Error::Ok;
        // Moved or hidden at the next round: what it covered put back then, no window asked.
        _bandId = screen.isEmpty() ? 0 : id;
        _band = screen;
        _bandMoved = true;
        changed(focusId());
        return Error::Ok;
}

namespace {

        // A band's piece, saved and drawn in one read and write of the surface.
        struct BandJob {
                        Rect             piece; // widened to 4-pixel columns
                        Rect             band;
                        Span<const Rect> clip; // where its window shows
                        uint8_t         *under;
                        uint32_t         dark;
                        uint32_t         light;
        };

        struct UnbandJob {
                        const uint8_t *under;
                        uint16_t       width;
                        uint16_t       rows;
        };

        constexpr uint16_t BandRing = 2;         // its thickness: a dark pixel outside, a light one inside
        constexpr uint16_t BandPieceWidth = 600; // a piece of its top or bottom (a patch holds a row's bytes)
        constexpr uint16_t BandPieceRows = 150;  // a piece of its sides (a group or two of 4 wide)

} // namespace

void WindowManager::bandPixels(void *arg, uint8_t *pixels, size_t stride) {
        const auto  *job = static_cast<const BandJob *>(arg);
        const Rect  &p = job->piece;
        const Rect  &b = job->band;
        const size_t rowBytes = size_t(p.width) * Surface::BytesPerPixel;
        for (uint16_t r = 0; r < p.height; ++r) std::memcpy(job->under + r * rowBytes, pixels + r * stride, rowBytes);
        for (uint16_t r = 0; r < p.height; ++r) {
                const uint32_t y = p.y + r;
                for (uint16_t c = 0; c < p.width; ++c) {
                        const uint32_t x = p.x + c;
                        if (x < b.x || y < b.y || x >= b.right() || y >= b.bottom()) continue;
                        // Its distance in from the band's edge: 0 the dark ring, 1 the light one.
                        const uint32_t in =
                                std::min(std::min(x - b.x, b.right() - 1 - x), std::min(y - b.y, b.bottom() - 1 - y));
                        if (in >= BandRing) continue;
                        bool shown = false;
                        for (const Rect &k : job->clip)
                                shown = shown || (x >= k.x && y >= k.y && x < k.right() && y < k.bottom());
                        if (!shown) continue;
                        const uint32_t rgb = in == 0 ? job->dark : job->light;
                        uint8_t       *px = pixels + r * stride + c * Surface::BytesPerPixel;
                        px[0] = static_cast<uint8_t>(rgb);
                        px[1] = static_cast<uint8_t>(rgb >> 8);
                        px[2] = static_cast<uint8_t>(rgb >> 16);
                }
        }
        return;
}

void WindowManager::unbandPixels(void *arg, uint8_t *pixels, size_t stride) {
        const auto  *job = static_cast<const UnbandJob *>(arg);
        const size_t rowBytes = size_t(job->width) * Surface::BytesPerPixel;
        for (uint16_t r = 0; r < job->rows; ++r) std::memcpy(pixels + r * stride, job->under + r * rowBytes, rowBytes);
        return;
}

// What the band covered put back (before anything is drawn this round, after the pointer's).
void WindowManager::restoreBand() {
        for (size_t i = _bandPieces.size(); i-- > 0;) {
                const BandPiece &piece = _bandPieces[i];
                UnbandJob        job{_bandUnder.data() + piece.offset, piece.rect.width, piece.rect.height};
                ++_stats.calls;
                if (_surface->patch(piece.rect, unbandPixels, &job, DrawTimeoutMs).isError()) ++_stats.errors;
        }
        _bandPieces.clear();
        return;
}

// The band over what was drawn this round, where its window shows: its rings in pieces (its top and
// bottom, then its sides), what each covers saved first.
void WindowManager::drawBand() {
        auto [z, zerr] = _stack.layerOf(_bandId);
        if (zerr.isError() || _band.width < 2 * BandRing || _band.height < 2 * BandRing) return;
        Region area;
        area.add(_band);
        ComposePiece pieces[32];
        Rect         clip[32];
        size_t       nc = 0;
        auto [n, perr] = _stack.plan(area, Span<ComposePiece>(pieces, 32));
        if (perr.isError()) return; // too broken up: not drawn this round
        for (size_t i = 0; i < n; ++i)
                if (pieces[i].layer == z) clip[nc++] = pieces[i].rect;
        if (nc == 0) return;
        // The rings' strips, widened to 4-pixel columns, in pieces a patch takes.
        const Rect &b = _band;
        const Rect  strips[] = {{b.x, b.y, b.width, BandRing},
                                {b.x, uint16_t(b.bottom() - BandRing), b.width, BandRing},
                                {b.x, uint16_t(b.y + BandRing), BandRing, uint16_t(b.height - 2 * BandRing)},
                                {uint16_t(b.right() - BandRing), uint16_t(b.y + BandRing), BandRing,
                                 uint16_t(b.height - 2 * BandRing)}};
        size_t      bytes = 0;
        _bandPieces.clear();
        for (const Rect &s : strips) {
                if (s.isEmpty()) continue;
                const uint32_t left = s.x & ~(Align - 1u), right = (s.right() + Align - 1u) & ~(Align - 1u);
                const bool     across = s.height == BandRing && s.width > BandRing;
                for (uint32_t x = left; x < right; x += across ? BandPieceWidth : right - left) {
                        for (uint32_t y = s.y; y < s.bottom(); y += across ? s.height : BandPieceRows) {
                                const uint32_t w =
                                        across ? std::min<uint32_t>(BandPieceWidth, right - x) : right - left;
                                const uint32_t h =
                                        across ? s.height : std::min<uint32_t>(BandPieceRows, s.bottom() - y);
                                const Rect piece{uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h)};
                                if (_bandPieces.append(BandPiece{piece, uint32_t(bytes)}).isError()) {
                                        _bandPieces.clear();
                                        return;
                                }
                                bytes += size_t(w) * h * Surface::BytesPerPixel;
                        }
                }
        }
        if (_bandUnder.size() < bytes) {
                if (_bandUnder.reserve(bytes).isError()) {
                        _bandPieces.clear();
                        return;
                }
                while (_bandUnder.size() < bytes) (void)_bandUnder.append(0);
        }
        for (size_t i = 0; i < _bandPieces.size(); ++i) {
                const BandPiece &piece = _bandPieces[i];
                BandJob          job{piece.rect,
                            b,
                            Span<const Rect>(clip, nc),
                            _bandUnder.data() + piece.offset,
                            _palette.color(Role::FaceDark),
                            _palette.color(Role::Outline)};
                ++_stats.calls;
                if (_surface->patch(piece.rect, bandPixels, &job, DrawTimeoutMs).isError()) {
                        ++_stats.errors;
                        _bandPieces.truncate(i); // those drawn are put back
                        return;
                }
        }
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
                        const Window &w = _windows[id - 1];
                        if (w.activated != nullptr) w.activated(w.arg, id == top);
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
                // The next free place down the cascade, among the windows shown (without
                // the memory for their list, the first place).
                List<Rect> taken;
                if (taken.reserve(_stack.count()).isOk()) {
                        for (size_t z = 0; z < _stack.count(); ++z)
                                if (WindowStack::isApplication(_stack.levelOf(_stack.idAt(z))))
                                        (void)taken.append(_stack.rectAt(z));
                }
                const auto width = static_cast<uint16_t>((r.width + Align - 1u) & ~(Align - 1u));
                r = cascadeRect(width, r.height, _workArea, taken.span());
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
        while (_windows.size() < id) {
                if (_windows.append(Window()).isError()) {
                        (void)_stack.close(id);
                        return makeError<uint8_t>(Error::NoMem);
                }
        }
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
        w.client = config.client <= _clients.size() ? config.client : 0;
        w.dump = config.dump;
        w.reference = config.reference;
        if (config.kind != nullptr) {
                std::strncpy(w.kind, config.kind, MaxKind);
                w.kind[MaxKind] = '\0';
        }
        w.mouse = config.mouse;
        w.reserve = config.level == Level::Panel ? config.reserve : 0;
        // A desktop's or a panel's window takes the keys only when pressed or raised (§4r).
        const bool keysLevel = config.level == Level::Desktop || config.level == Level::Panel;
        w.focusable = config.focusable && (WindowStack::isApplication(config.level) || keysLevel);
        w.focusOnPress = config.focusOnPress;
        w.activated = config.activated;
        if (w.focusable && !keysLevel) _focusId = id;
        if (keysLevel && _focusId == id) _focusId = 0; // a closed window's number, reused: not focused
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
        if (_bandId == id) {
                // Its area is redrawn as the window goes: nothing of the band to put back.
                _bandId = 0;
                _bandPieces.clear();
        }
        if (w.closed != nullptr) w.closed(w.arg);
        if (_grabId == id) _grabId = 0;
        if (_sizeId == id) _sizeId = 0;
        if (_pressId == id) _pressId = 0;
        if (_clickId == id) _clickId = 0;
        if (_mouseId == id) _mouseId = 0;
        if (_hoverId == id) _hoverId = 0;
        if (_unsnapId == id) _unsnapId = 0;
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
        if (w.maximized || w.shaded) return Error::InvalidState;
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

Error WindowManager::raiseLocked(uint8_t id, bool focus) {
        const uint8_t oldTop = focusId();
        const bool    shown = _stack.isHidden(id);
        const Error   err = shown ? _stack.show(id) : _stack.raise(id);
        if (err.isError()) return err;
        if (focus && _windows[id - 1].focusable) _focusId = id;
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
        if (!_canMinimize) return Error::InvalidState;                                   // nothing would bring it back
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
        if (w.shaded) (void)shadeLocked(id, false);
        auto [was, rerr] = _stack.rectOf(id);               // where the drop put it
        if (!w.snapped) w.restore = rerr.isOk() ? was : at; // a snapped one's is from before it snapped
        w.snapped = false;
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
        if (w.shaded) (void)shadeLocked(id, false);
        if (!w.maximized && !w.snapped) return Error::Ok;
        w.maximized = false;
        w.snapped = false;
        _listChanged = true;
        return resizeLocked(id, placed(w.restore), w.decorations);
}

Result<uint8_t> WindowManager::connect(Gone gone, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return makeError<uint8_t>(Error::InvalidState); // no window system
        for (size_t i = 0; i < _clients.size(); ++i) {
                if (_clients[i].used) continue;
                _clients[i] = Client{gone, arg, true};
                return makeResult(static_cast<uint8_t>(i + 1));
        }
        if (_clients.size() == MaxClients) return makeError<uint8_t>(Error::Full);
        if (_clients.append(Client{gone, arg, true}).isError()) return makeError<uint8_t>(Error::NoMem);
        return makeResult(static_cast<uint8_t>(_clients.size()));
}

void WindowManager::disconnect(uint8_t client) {
        Mutex::Locker lock(_lock, WaitForever);
        if (client == 0 || client > _clients.size() || !_clients[client - 1].used) return;
        _clients[client - 1] = Client();
        for (KeyGrab &g : _grabs)
                if (g.client == client) g = KeyGrab();
        dropServices(client);
        // Its windows, shown or hidden, closed.
        for (size_t id = 1; id <= _windows.size(); ++id)
                if (_windows[id - 1].client == client && _stack.rectOf(uint8_t(id)).isOk())
                        (void)closeLocked(uint8_t(id));
        watchersChanged(); // the panel gone: nothing to minimize to
        return;
}

Error WindowManager::watch(uint8_t client, Watch fn, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (client == 0 || client > _clients.size() || !_clients[client - 1].used) return Error::NotFound;
        _clients[client - 1].watch = fn;
        _clients[client - 1].watchArg = arg;
        watchersChanged();
        return Error::Ok;
}

bool WindowManager::canMinimize() {
        Mutex::Locker lock(_lock, WaitForever);
        return _canMinimize;
}

// Whether a program watches the window list (the panel: windows can be minimized) changed: the
// minimize buttons redrawn; without one, the minimized windows shown again.
void WindowManager::watchersChanged() {
        bool now = false;
        for (const Client &c : _clients) now = now || (c.used && c.watch != nullptr);
        if (now == _canMinimize) return;
        _canMinimize = now;
        for (size_t z = 0; z < _stack.count(); ++z) {
                const uint8_t id = _stack.idAt(z);
                const Rect    r = frame(id, _stack.rectAt(z)).button(WindowFrame::Part::Minimize);
                if (!r.isEmpty()) (void)_stack.invalidate(id, r);
        }
        if (!now) {
                // Each raise takes one from the hidden: the one at i is the next.
                for (size_t i = 0; i < _stack.hiddenCount();) {
                        const uint8_t id = _stack.hiddenIdAt(i);
                        if (!WindowStack::isApplication(_stack.levelOf(id)) || raiseLocked(id).isError() ||
                            _stack.isHidden(id))
                                ++i;
                }
        }
        changed(focusId());
        return;
}

// The focus: the focusable window raised last (an always-on-top window above it doesn't
// take it), or, when that one went, the top focusable window.
uint8_t WindowManager::focusId() const {
        if (_focusId != 0 && _windows[_focusId - 1].focusable && _stack.layerOf(_focusId).isOk()) return _focusId;
        // The top application window; without one, a desktop window that takes keys (§4r).
        uint8_t desktop = 0;
        for (size_t z = 0; z < _stack.count(); ++z) {
                const uint8_t id = _stack.idAt(z);
                if (!_windows[id - 1].focusable) continue;
                const Level level = _stack.levelOf(id);
                if (WindowStack::isApplication(level)) return id;
                if (level == Level::Desktop && desktop == 0) desktop = id;
        }
        return desktop;
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
        event.modifiers = _modifiers;
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
        for (size_t id = 1; id <= _windows.size(); ++id) {
                const Window &w = _windows[id - 1];
                if (w.maximized && _stack.rectOf(uint8_t(id)).isOk())
                        (void)resizeLocked(uint8_t(id), placed(area), w.decorations);
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
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        if (_menuId != 0) (void)closeLocked(_menuId);
        const Window &w = _windows[id - 1];
        const bool    hidden = _stack.isHidden(id);
        const auto    off = [](bool enabled) {
                return enabled ? uint8_t(0) : uint8_t(MenuLayout::Disabled);
        };
        const bool app = WindowStack::isApplication(_stack.levelOf(id));
        const bool above = _stack.levelOf(id) == Level::Above;
        setMenuItem(0, "&Restore", "", off(hidden || w.maximized || w.snapped));
        setMenuItem(1, "Mi&nimize", "Win+Down",
                    off(_canMinimize && !hidden && (w.decorations & WindowFrame::MinimizeButton) != 0));
        setMenuItem(2, "Ma&ximize", "Win+Up", off(!w.maximized && (w.decorations & WindowFrame::MaximizeButton) != 0));
        setMenuItem(3, w.shaded ? "Roll &Down" : "Roll &Up", "",
                    off(app && !hidden && (w.decorations & WindowFrame::TitleBar) != 0));
        setMenuItem(4, "", "", MenuLayout::Separator);
        setMenuItem(5, "Always on &Top", "", static_cast<uint8_t>(off(app) | (above ? MenuLayout::Checked : 0)));
        setMenuItem(6, "", "", MenuLayout::Separator);
        setMenuItem(7, "&Close", "Alt+F4", off((w.decorations & WindowFrame::CloseButton) != 0));
        constexpr MenuChoice choices[] = {MenuChoice::Restore, MenuChoice::Minimize, MenuChoice::Maximize,
                                          MenuChoice::Shade,   MenuChoice::None,     MenuChoice::Above,
                                          MenuChoice::None,    MenuChoice::Close};
        constexpr size_t     Count = sizeof(choices) / sizeof(choices[0]);
        for (size_t i = 0; i < Count; ++i) _menuChoices[i] = choices[i];
        _menuCount = Count;
        Error err = _menuLayout.layout(Span<const MenuLayout::Item>(_menuItems, Count), Fonts::unscii16);
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
        drawMenu(painter, self->_menuLayout, Span<const MenuLayout::Item>(self->_menuItems, self->_menuCount),
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

// Item @p item chosen: the menu closed, then done to its window (or, the desktop's, told).
void WindowManager::menuChoose(int item) {
        const uint8_t    id = _menuFor;
        const MenuChoice choice = item >= 0 && size_t(item) < _menuCount ? _menuChoices[item] : MenuChoice::None;
        if (_menuId != 0) (void)closeLocked(_menuId);
        if (choice == MenuChoice::Desktop) {
                _desktopPicked = item; // told after this round, the lock released
                return;
        }
        if (choice == MenuChoice::Program) {
                _programPicked = item; // told after this round, the lock released
                _pickedFn = _programChosen;
                _pickedArg = _programArg;
                return;
        }
        if (id == 0 || _stack.rectOf(id).isError()) return;
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
                case MenuChoice::Shade: (void)shadeLocked(id, !_windows[id - 1].shaded); break;
                case MenuChoice::Close: (void)closeLocked(id); break;
                case MenuChoice::Desktop:
                case MenuChoice::Program:
                case MenuChoice::None: break;
        }
        return;
}

// The desktop's menu (setDesktopMenu()) at (@p x, @p y).
Error WindowManager::showDesktopMenuLocked(uint16_t x, uint16_t y) {
        if (_desktopCount == 0) return Error::NotFound;
        if (_menuId != 0) (void)closeLocked(_menuId);
        for (size_t i = 0; i < _desktopCount; ++i) {
                setMenuItem(i, StringView(_desktopLabels[i]), StringView(), 0);
                _menuChoices[i] = MenuChoice::Desktop;
        }
        _menuCount = _desktopCount;
        return openMenuLocked("Desktop menu", Rect{x, y, 0, 0}, 0);
}

// The menu of _menuItems (_menuCount of them) opened at @p anchor, for window @p menuFor (0: the
// desktop).
Error WindowManager::openMenuLocked(const char *title, const Rect &anchor, uint8_t menuFor) {
        Error err = _menuLayout.layout(Span<const MenuLayout::Item>(_menuItems, _menuCount), Fonts::unscii16);
        if (err.isError()) return err;
        WindowConfig config;
        config.rect = MenuLayout::place(_menuLayout.width(), _menuLayout.height(), anchor, _stack.screen(), false);
        config.title = title;
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
        _menuFor = menuFor;
        _menuHighlight = -1;
        return Error::Ok;
}

Error WindowManager::showMenu(uint8_t id, Span<const MenuItem> items, uint16_t x, uint16_t y, MenuChosen fn,
                              void *arg) {
        static_assert(MaxMenuItems <= MenuItems);
        if (items.isEmpty() || items.size() > MaxMenuItems) return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return Error::NotFound;
        if (_menuId != 0) (void)closeLocked(_menuId);
        for (size_t i = 0; i < items.size(); ++i) {
                // The shortcut cut on a character's boundary.
                const StringView text = items[i].shortcut;
                size_t           n = text.size() < MaxMenuShortcut ? text.size() : MaxMenuShortcut;
                while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
                std::memcpy(_programShortcuts[i], text.data(), n);
                _programShortcuts[i][n] = '\0';
                setMenuItem(i, items[i].label, StringView(_programShortcuts[i], n), items[i].flags);
                _menuChoices[i] = MenuChoice::Program;
        }
        _menuCount = items.size();
        _programChosen = fn;
        _programArg = arg;
        const Rect client = frame(id, at).client();
        return openMenuLocked("Menu", Rect{uint16_t(client.x + x), uint16_t(client.y + y), 0, 0}, id);
}

Error WindowManager::setDesktopMenu(Span<const StringView> labels, DesktopChosen fn, void *arg) {
        if (labels.size() > MaxDesktopItems) return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        for (size_t i = 0; i < labels.size(); ++i) {
                // Cut on a character's boundary.
                size_t n = labels[i].size() < MaxDesktopLabel ? labels[i].size() : MaxDesktopLabel;
                while (n > 0 && n < labels[i].size() && (uint8_t(labels[i][n]) & 0xc0) == 0x80) --n;
                std::memcpy(_desktopLabels[i], labels[i].data(), n);
                _desktopLabels[i][n] = '\0';
        }
        _desktopCount = fn != nullptr ? labels.size() : 0;
        _desktopChosen = fn;
        _desktopArg = arg;
        return Error::Ok;
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

// Item @p i of the menu to open: @p label's marks taken out (into _menuLabels), its key kept.
void WindowManager::setMenuItem(size_t i, StringView label, StringView shortcut, uint8_t flags) {
        const Mnemonic::Stripped m = Mnemonic::strip(label, Span<char>(_menuLabels[i], MaxMenuLabel + 1));
        _menuItems[i] = MenuLayout::Item{StringView(_menuLabels[i], m.length), shortcut, flags, nullptr, m.at};
        _menuKeys[i] = m.key;
        return;
}

// A key typed in the menu: the one item it marks chosen; with several, the next of them lit.
void WindowManager::menuMnemonic(uint32_t key) {
        int    first = -1, after = -1;
        size_t count = 0;
        for (size_t i = 0; i < _menuCount; ++i) {
                if (_menuKeys[i] != key || !_menuLayout.isSelectable(i)) continue;
                ++count;
                if (first < 0) first = int(i);
                if (after < 0 && int(i) > _menuHighlight) after = int(i);
        }
        if (count == 1) {
                menuChoose(first);
        } else if (count > 1) {
                menuHighlight(after >= 0 ? after : first);
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
        } else if (const uint32_t key = Mnemonic::keyOf(event); key != 0) {
                self->menuMnemonic(key);
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

// ---- Snapping and rolling up (devplan/gui.md §4k)

namespace {

        constexpr uint16_t SnapEdge = 2; // pixels from the work area's side that snap

} // namespace

// Where a window grabbed by its title bar snaps with the pointer at (@p x, @p y): the work
// area's top edge (or above it, on a panel) maximizes, a side gives that half. Only a
// window that can be maximized, or (sides) resized.
WindowManager::Snap WindowManager::snapAt(uint8_t id, uint16_t x, uint16_t y) const {
        const Window &w = _windows[id - 1];
        if (!WindowStack::isApplication(_stack.levelOf(id))) return Snap::None;
        const Rect a = _workArea;
        if (y <= a.y && (w.decorations & WindowFrame::MaximizeButton) != 0) return Snap::Maximize;
        if ((w.decorations & WindowFrame::Resizable) == 0 || y < a.y) return Snap::None;
        if (x < a.x + SnapEdge) return Snap::Left;
        if (x + SnapEdge >= a.right()) return Snap::Right;
        return Snap::None;
}

Rect WindowManager::snapRect(Snap snap) const {
        const Rect     a = _workArea;
        const uint16_t half = static_cast<uint16_t>((a.width / 2) & ~(Align - 1u));
        switch (snap) {
                case Snap::Left: return Rect{a.x, a.y, half, a.height};
                case Snap::Right: return Rect{uint16_t(a.x + half), a.y, uint16_t(a.width - half), a.height};
                default: return placed(a);
        }
}

// The outline of a move or resize under way, gone.
void WindowManager::hideOutline() {
        if (_outlineShown) {
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
        }
        _dragId = 0;
        _outlineShown = false;
        return;
}

// Window @p id snapped (@p snap): maximized, or half the work area (its size and place kept to
// go back to).
Error WindowManager::snapLocked(uint8_t id, Snap snap) {
        if (snap == Snap::Maximize) return maximizeLocked(id);
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        Window &w = _windows[id - 1];
        if (w.shaded) (void)shadeLocked(id, false);
        if (!w.snapped && !w.maximized) w.restore = at;
        w.maximized = false;
        w.snapped = true;
        _listChanged = true;
        err = raiseLocked(id);
        if (err.isOk()) err = resizeLocked(id, snapRect(snap), w.decorations);
        return err;
}

Error WindowManager::shade(uint8_t id, bool shaded) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return shadeLocked(id, shaded);
}

// Window @p id rolled up to its frame's top (title bar) and bottom, or down again; its
// application isn't told (its client area keeps its size, hidden).
Error WindowManager::shadeLocked(uint8_t id, bool shaded) {
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        Window &w = _windows[id - 1];
        if (!WindowStack::isApplication(_stack.levelOf(id)) || _stack.isHidden(id) ||
            (w.decorations & WindowFrame::TitleBar) == 0)
                return Error::InvalidState;
        if (w.shaded == shaded) return Error::Ok;
        if (_dragId == id) hideOutline();
        if (_sizeId == id) _sizeId = 0;
        Rect to = at;
        if (shaded) {
                const Rect client = WindowFrame(at, w.decorations).client();
                w.shadeHeight = at.height;
                to.height = static_cast<uint16_t>(at.height - client.height);
        } else {
                to.height = w.shadeHeight;
                to = placed(to);
        }
        w.shaded = shaded;
        _listChanged = true;
        if (_mouseId == id) _mouseId = 0;
        return resizeLocked(id, to, w.decorations, false);
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
        constexpr uint8_t  UsageF10 = HidKey::F1 + 9;
        constexpr uint8_t  UsageLeftAlt = 0xe2;
        constexpr uint8_t  UsageRightAlt = 0xe6;
        constexpr uint16_t SwitcherWidth = 320; // a box in the screen's middle, not a menu

} // namespace

// Alt+Tab's list: the windows in it (their icons looked up as it is drawn, with the lock held:
// their owners set them under it), their titles as shown, which one is chosen.
struct WindowManager::Switcher {
                SwitcherLayout layout;
                char           texts[WindowStack::MaxWindows][MaxTitle + 3];
                uint8_t        ids[WindowStack::MaxWindows];
                uint32_t       doubled[SwitcherLayout::IconSide * SwitcherLayout::IconSide]; // a small icon, x2
                size_t         count = 0;
                int            at = 0;
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
        // A key pressed while a claimed modifier is down: that modifier's press is no tap.
        if (press && (event.usage < 0xe0 || event.usage > 0xe7)) _tapClean = false;
        if (windowKey(event)) return true;
        if (!press || !alt || event.hasCtrl() || (event.modifiers & KeyEvent::Gui) != 0) return grabbed(event);
        if (event.usage == HidKey::Tab) {
                switchOpen(event.hasShift() ? -1 : 1);
                return true;
        }
        const uint8_t focus = focusId();
        if (event.usage == UsageF4 && focus != 0) {
                if ((_windows[focus - 1].decorations & WindowFrame::CloseButton) != 0) (void)closeLocked(focus);
                return true;
        }
        if (event.usage == HidKey::Space && focus != 0 && WindowStack::isApplication(_stack.levelOf(focus))) {
                auto [at, err] = _stack.rectOf(focus);
                if (err.isOk()) {
                        const WindowFrame f = frame(focus, at);
                        const Rect        bar = f.has(WindowFrame::TitleBar) ? f.titleBar() : f.client();
                        (void)showWindowMenuLocked(focus, Rect{bar.x, bar.y, 0, bar.height});
                }
                return true;
        }
        return grabbed(event);
}

Error WindowManager::grabKey(uint8_t client, uint8_t usage, uint8_t modifiers, Grab fn, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (client == 0 || client > _clients.size() || !_clients[client - 1].used || fn == nullptr)
                return Error::NotFound;
        const auto folded = static_cast<uint8_t>((modifiers | modifiers >> 4) & 0x0f);
        KeyGrab   *free = nullptr;
        for (KeyGrab &g : _grabs) {
                if (g.client != 0 && g.usage == usage && g.modifiers == folded) return Error::Exists;
                if (g.client == 0 && free == nullptr) free = &g;
        }
        if (free == nullptr) return Error::Full;
        *free = KeyGrab{fn, arg, client, usage, folded};
        return Error::Ok;
}

// ---- Services (devplan/filebrowser.md §4h, FB6)

WindowManager::Service *WindowManager::findService(StringView name) {
        for (Service *s : _services)
                if (StringView(s->name) == name) return s;
        return nullptr;
}

// Connection @p client's services (0: every one), forgotten with their messages.
void WindowManager::dropServices(uint8_t client) {
        for (size_t i = _services.size(); i-- > 0;) {
                Service *s = _services[i];
                if (client != 0 && s->client != client) continue;
                s->~Service();
                Memory::free(s);
                _services.removeAt(i);
        }
        return;
}

Error WindowManager::registerService(uint8_t client, StringView name, ServiceNotify fn, void *arg) {
        if (name.isEmpty() || name.size() > MaxServiceName || fn == nullptr) return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (client == 0 || client > _clients.size() || !_clients[client - 1].used) return Error::NotFound;
        if (findService(name) != nullptr) return Error::Exists;
        if (_services.reserve(_services.size() + 1).isError()) return Error::NoMem;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Service));
        if (mem == nullptr) return Error::NoMem;
        auto *s = ::new (mem) Service();
        std::memcpy(s->name, name.data(), name.size());
        s->client = client;
        s->fn = fn;
        s->arg = arg;
        (void)_services.append(s);
        return Error::Ok;
}

Error WindowManager::sendToService(StringView name, ByteSpan message) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        Service *s = findService(name);
        if (s == nullptr) return Error::NotFound;
        const Error err = s->queue.push(message);
        if (err.isOk()) s->fn(s->arg);
        return err;
}

Result<size_t> WindowManager::takeServiceMessage(uint8_t client, StringView name, WritableByteSpan out) {
        Mutex::Locker lock(_lock, WaitForever);
        Service      *s = findService(name);
        if (s == nullptr || s->client != client) return makeError<size_t>(Error::NotFound);
        return s->queue.take(out);
}

// The window keys (devplan/gui.md §4p): Win+Up/Down/Left/Right, Alt+F10 on the focused
// application window; Ctrl+Shift+Windows (held together, in any order) for win start's command.
bool WindowManager::windowKey(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const bool modifier = event.usage >= 0xe0 && event.usage <= 0xe7;
        // A modifier's own bit (left Ctrl 0xe0 is bit 0 ... right Gui 0xe7 bit 7), in case the
        // event's modifiers don't count it yet.
        const auto mods = static_cast<uint8_t>(event.modifiers | (modifier ? 1u << (event.usage - 0xe0) : 0u));
        const bool ctrl = (mods & KeyEvent::Ctrl) != 0, shift = (mods & KeyEvent::Shift) != 0;
        const bool alt = (mods & KeyEvent::Alt) != 0, gui = (mods & KeyEvent::Gui) != 0;
        if (modifier) {
                if (!ctrl || !shift || !gui || alt || event.action != KeyEvent::Action::Press) return false;
                _tapClean = false;                // no launcher with it
                _desktopPicked = int(DesktopKey); // told after the round, unlocked
                return true;
        }
        const uint8_t focus = focusId();
        const bool    winKey = gui && !ctrl && !shift && !alt;
        const bool    arrow = event.usage == HidKey::Up || event.usage == HidKey::Down || event.usage == HidKey::Left ||
                           event.usage == HidKey::Right;
        if (!(winKey && arrow) && !(alt && !gui && !ctrl && !shift && event.usage == UsageF10)) return false;
        if (focus == 0 || !WindowStack::isApplication(_stack.levelOf(focus))) return true;
        Window       &w = _windows[focus - 1];
        const uint8_t d = w.decorations;
        if (event.usage == UsageF10) {
                if ((d & WindowFrame::MaximizeButton) != 0)
                        (void)(w.maximized ? restoreLocked(focus) : maximizeLocked(focus));
        } else if (event.usage == HidKey::Up) {
                if ((d & WindowFrame::MaximizeButton) != 0 && !w.maximized) (void)maximizeLocked(focus);
        } else if (event.usage == HidKey::Down) {
                if (w.maximized || w.snapped) {
                        (void)restoreLocked(focus);
                } else if ((d & WindowFrame::MinimizeButton) != 0) {
                        (void)minimizeLocked(focus); // refused with nowhere to go
                }
        } else if ((d & WindowFrame::Resizable) != 0) {
                // Left or Right: snapped to that half; again (already there), back.
                const Snap snap = event.usage == HidKey::Left ? Snap::Left : Snap::Right;
                auto [at, err] = _stack.rectOf(focus);
                if (err.isOk() && w.snapped && at == snapRect(snap)) {
                        (void)restoreLocked(focus);
                } else {
                        (void)snapLocked(focus, snap);
                }
        }
        return true;
}

// @p event, if a program claimed it (grabKey()): it is told, and the key isn't delivered.
bool WindowManager::grabbed(const KeyEvent &event) {
        const bool    press = event.action != KeyEvent::Action::Release;
        const bool    modifier = event.usage >= 0xe0 && event.usage <= 0xe7;
        const uint8_t folded = static_cast<uint8_t>((event.modifiers | event.modifiers >> 4) & 0x0f);
        for (KeyGrab &g : _grabs) {
                if (g.client == 0 || g.usage != event.usage) continue;
                if (modifier && g.modifiers == 0) {
                        // A tap: its press noted (and kept from windows), told at its release if clean.
                        if (event.action == KeyEvent::Action::Press) {
                                _tapUsage = event.usage;
                                _tapClean = true;
                        } else if (!press && _tapUsage == event.usage) {
                                const bool tap = _tapClean;
                                _tapUsage = 0;
                                _tapClean = false;
                                if (tap) g.fn(g.arg, event.usage);
                        }
                        return true;
                }
                if (press && g.modifiers == folded) {
                        if (event.action == KeyEvent::Action::Press) g.fn(g.arg, event.usage);
                        return true;
                }
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
                std::snprintf(sw->texts[sw->count], sizeof(sw->texts[0]), _stack.isHidden(id) ? "[%s]" : "%s", title);
                sw->ids[sw->count++] = id;
        }
        if (sw->count == 0) {
                Memory::free(sw);
                return;
        }
        sw->layout.layout(sw->count, _stack.screen().width);
        sw->at = sw->count == 1 ? 0 : (step > 0 ? 1 : int(sw->count) - 1);
        const Rect     screen = _stack.screen();
        const uint16_t w = sw->layout.width();
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

// The list's choice moved to @p item (around the ends): the two cells and the title redrawn.
void WindowManager::switchTo(int item) {
        Switcher *sw = _switcher;
        if (sw == nullptr) return;
        const int n = int(sw->count);
        item = ((item % n) + n) % n;
        if (item == sw->at) return;
        const int old = sw->at;
        sw->at = item;
        // The two cells and the title.
        auto [at, err] = _stack.rectOf(_switchId);
        if (err.isOk()) {
                for (const Rect &r :
                     {sw->layout.cellRect(size_t(sw->at)), sw->layout.cellRect(size_t(old)), sw->layout.titleRect()})
                        (void)_stack.invalidate(_switchId,
                                                Rect{uint16_t(at.x + r.x), uint16_t(at.y + r.y), r.width, r.height});
        }
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

// The box: a raised face, the icons (the chosen one's cell lit), the chosen window's title in a
// sunken field.
void WindowManager::switchRedraw(void *arg, Painter &painter) {
        auto *self = static_cast<WindowManager *>(arg);
        if (self->_switcher == nullptr) return;
        Switcher             &sw = *self->_switcher;
        const SwitcherLayout &l = sw.layout;
        const WindowPalette  &palette = painter.palette();
        const uint32_t        face = palette.color(Role::Face);
        const Rect            box{0, 0, l.width(), l.height()};
        drawBevel(painter, box, Bevel::Style::Window);
        (void)painter.fill(Bevel::inside(box, Bevel::Style::Window), face);
        for (size_t i = 0; i < sw.count; ++i) {
                const Rect cell = l.cellRect(i);
                if (!painter.isVisible(cell)) continue;
                const uint32_t bg = int(i) == sw.at ? palette.color(Role::Highlight) : face;
                (void)painter.fill(cell, bg);
                const Window &w = self->_windows[sw.ids[i] - 1];
                const Rect    r = l.iconRect(i);
                const Icon   *big = w.largeIcon;
                const Icon   *small = w.icon;
                if (big != nullptr && big->isValid()) {
                        (void)painter.image(static_cast<uint16_t>(r.x + ((r.width - big->width) / 2 & ~3u)),
                                            static_cast<uint16_t>(r.y + (r.height - big->height) / 2), *big, bg);
                } else if (small != nullptr && small->isValid() && small->width * 2 <= r.width &&
                           small->height * 2 <= r.height) {
                        // Doubled, each pixel two by two.
                        const uint16_t dw = small->width * 2, dh = small->height * 2;
                        for (uint16_t y = 0; y < dh; ++y)
                                for (uint16_t x = 0; x < dw; ++x)
                                        sw.doubled[size_t(y) * dw + x] =
                                                small->pixels[size_t(y / 2) * small->width + x / 2];
                        (void)painter.image(static_cast<uint16_t>(r.x + ((r.width - dw) / 2 & ~3u)),
                                            static_cast<uint16_t>(r.y + (r.height - dh) / 2), Icon{dw, dh, sw.doubled},
                                            bg);
                } else {
                        // None: a window drawn small (a frame, a title bar, its client area).
                        const Rect outer{static_cast<uint16_t>(r.x + 2), static_cast<uint16_t>(r.y + 4), 28, 24};
                        const Rect bar{static_cast<uint16_t>(outer.x + 1), static_cast<uint16_t>(outer.y + 1), 26, 5};
                        const Rect body{static_cast<uint16_t>(outer.x + 1), static_cast<uint16_t>(outer.y + 6), 26, 17};
                        (void)painter.fill(outer, palette.color(Role::FaceDark));
                        (void)painter.fill(bar, palette.color(Role::ActiveFrame));
                        (void)painter.fill(body, palette.color(Role::Client));
                }
        }
        const Rect title = l.titleRect();
        if (painter.isVisible(title)) {
                drawBevel(painter, title, Bevel::Style::Field);
                const Rect     in = Bevel::inside(title, Bevel::Style::Field);
                const uint32_t fg = palette.color(Role::FaceText);
                (void)painter.fill(in, face);
                const StringView text(sw.texts[sw.at]);
                const uint16_t   room = static_cast<uint16_t>(in.width > 8 ? in.width - 8 : 0);
                const StringView shown = text.left(Fonts::unscii16.fitting(text, room));
                const uint32_t   tw = Fonts::unscii16.textWidth(shown);
                const uint16_t   tx = static_cast<uint16_t>((in.x + (in.width - tw) / 2) & ~3u);
                const uint16_t   ty = static_cast<uint16_t>(in.y + (in.height - Fonts::unscii16.height()) / 2);
                (void)painter.text(tx, ty, shown, Fonts::unscii16, fg, face, room);
        }
        return;
}

void WindowManager::switchMouse(void *arg, const MouseEvent &event) {
        auto *self = static_cast<WindowManager *>(arg);
        if (self->_switcher == nullptr) return;
        const int item = self->_switcher->layout.cellAt(event.x, event.y);
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
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        const Window &w = _windows[id - 1];
        if (w.dump == nullptr) return Error::NotSupported;
        w.dump(w.arg, out);
        return Error::Ok;
}

Error WindowManager::reference(uint8_t id, uint8_t *pixels, size_t stride, uint16_t width, uint16_t height) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
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

Error WindowManager::setPointer(uint8_t id, Pointer shape) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        _windows[id - 1].pointer = shape;
        const Pointer now = shapeAt(_pointerX, _pointerY);
        if (now == _shape) return Error::Ok;
        _shape = now;
        if (isCursorDrawn()) _cursorMoved = true; // drawn again in its new shape
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::setIcon(uint8_t id, const Icon *icon) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        Window &w = _windows[id - 1];
        w.icon = icon;
        if (WindowStack::isApplication(_stack.levelOf(id))) _listChanged = true;
        auto [at, err] = _stack.rectOf(id);
        if (err.isOk() && !_stack.isHidden(id)) {
                const Rect button = frame(id, at).button(WindowFrame::Part::Menu);
                if (!button.isEmpty()) (void)_stack.invalidate(id, button);
        }
        changed(focusId());
        return Error::Ok;
}

Error WindowManager::setLargeIcon(uint8_t id, const Icon *icon) {
        if (icon != nullptr && (icon->width > SwitcherLayout::IconSide || icon->height > SwitcherLayout::IconSide))
                return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        if (_stack.rectOf(id).isError()) return Error::NotFound;
        _windows[id - 1].largeIcon = icon;
        return Error::Ok;
}

Result<WindowManager::Icon> WindowManager::copyIcon(uint8_t id, Span<uint32_t> out) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return makeError<Icon>(Error::InvalidState);
        if (_stack.rectOf(id).isError()) return makeError<Icon>(Error::NotFound);
        const Icon *icon = _windows[id - 1].icon;
        if (icon == nullptr || !icon->isValid()) return makeError<Icon>(Error::NotSupported);
        const size_t n = size_t(icon->width) * icon->height;
        if (n > out.size()) return makeError<Icon>(Error::BufferTooSmall);
        std::memcpy(out.data(), icon->pixels, n * sizeof(uint32_t));
        return makeResult(Icon{icon->width, icon->height, out.data()});
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
Error WindowManager::resizeLocked(uint8_t id, const Rect &rect, uint8_t decorations, bool tell) {
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
        if (tell && (after.width != before.width || after.height != before.height) && w.resized != nullptr)
                w.resized(w.arg, after.width, after.height);
        if (w.reserve != 0) workAreaChanged();
        changed(oldTop);
        return Error::Ok;
}

Error WindowManager::invalidate(uint8_t id, const Rect &rect) {
        return invalidate(id, Span<const Rect>(&rect, 1));
}

Error WindowManager::invalidate(uint8_t id, Span<const Rect> rects) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [at, err] = _stack.rectOf(id);
        if (err.isError()) return err;
        if (_stack.isHidden(id)) return Error::Ok; // drawn when shown
        const Rect client = frame(id, at).client();
        const Rect local{0, 0, client.width, client.height};
        bool       any = false;
        for (const Rect &rect : rects) {
                const Rect r = rect.intersected(local);
                if (r.isEmpty()) continue;
                // Out to 4-pixel columns (as the painters' damage must be: a text's copy is clipped
                // by it), kept in the client area.
                const uint32_t left = (uint32_t(client.x) + r.x) & ~(Align - 1u);
                uint32_t       right = (uint32_t(client.x) + r.right() + Align - 1u) & ~(Align - 1u);
                const uint32_t from = left > client.x ? left : client.x;
                if (right > client.right()) right = client.right();
                (void)_stack.invalidate(
                        id, Rect{uint16_t(from), uint16_t(client.y + r.y), uint16_t(right - from), r.height});
                any = true;
        }
        if (any) changed(focusId());
        return Error::Ok;
}

Error WindowManager::update(uint8_t id) {
        if (id == 0) return Error::NotFound;
        // Without the lock: a writer on another task doesn't wait for a round.
        _asked[id / 32].fetch_or(1u << (id % 32), std::memory_order_acq_rel);
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

Result<List<WindowManager::Info>> WindowManager::windows() {
        Mutex::Locker lock(_lock, WaitForever);
        const size_t  shown = _stack.count();
        List<Info>    out(MemSpace::Psram);
        if (out.reserve(shown + _stack.hiddenCount()).isError()) return makeError<List<Info>>(Error::NoMem);
        for (size_t i = 0; i < shown + _stack.hiddenCount(); ++i) {
                Info info;
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
                info.shaded = w.shaded;
                (void)out.append(info); // reserved
        }
        return makeResult(std::move(out));
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
