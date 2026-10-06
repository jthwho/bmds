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

#include <cstring>
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
        _closeId = 0;
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
        for (size_t z = 0; z < _stack.count(); ++z) {
                Window &w = _windows[_stack.idAt(z) - 1];
                if (w.closed != nullptr) w.closed(w.arg);
                w = Window();
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
                                deliverKeys();
                                applyPointer();
                                frame = surface->frames();
                                round();
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
        {
                SpinLock::Locker keys(_inputLock);
                if (_keyCount == KeyQueue) return; // nobody is taking them
                _keys[(_keyHead + _keyCount) % KeyQueue] = event;
                ++_keyCount;
        }
        _wake.release();
        return;
}

// The keys queued, to the focused window. With the lock held.
void WindowManager::deliverKeys() {
        for (;;) {
                KeyEvent event;
                {
                        SpinLock::Locker keys(_inputLock);
                        if (_keyCount == 0) return;
                        event = _keys[_keyHead];
                        _keyHead = static_cast<uint8_t>((_keyHead + 1) % KeyQueue);
                        --_keyCount;
                }
                if (_stack.count() == 0) continue;
                const Window &w = _windows[_stack.idAt(0) - 1];
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
                if (nx != _pointerX || ny != _pointerY || !_cursorShown) {
                        if (_cursorShown) _stack.invalidateScreen(cursorArea()); // what it covered, redrawn
                        _pointerX = nx;
                        _pointerY = ny;
                        _cursorShown = true;
                        _cursorMoved = true;
                        if (_grabId != 0) {
                                const int wx = int(nx) - _grabX, wy = int(ny) - _grabY;
                                (void)dragLocked(_grabId, static_cast<uint16_t>(wx < 0 ? 0 : wx),
                                                 static_cast<uint16_t>(wy < 0 ? 0 : wy));
                        }
                }
                const uint8_t pressed = static_cast<uint8_t>(event.buttons & ~_buttons);
                const uint8_t released = static_cast<uint8_t>(_buttons & ~event.buttons);
                _buttons = event.buttons;
                if ((pressed & PointerEvent::Left) != 0) press(nx, ny);
                if ((released & PointerEvent::Left) != 0) release(nx, ny);
                _requests.fetch_add(1, std::memory_order_acq_rel);
        }
}

// The left button down at (@p x, @p y): the window there raised; on its
// title bar, grabbed to move; on its close box, closed if released there.
void WindowManager::press(uint16_t x, uint16_t y) {
        const uint8_t id = windowAt(x, y);
        if (id == 0) return;
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return;
        const Rect outer = _stack.rectAt(z);
        const Rect at{x, y, 1, 1};
        (void)raiseLocked(id);
        if (WindowFrame::closeBox(outer).contains(at)) {
                _closeId = id;
        } else if (WindowFrame::titleBar(outer).contains(at)) {
                _grabId = id;
                _grabX = static_cast<uint16_t>(x - outer.x);
                _grabY = static_cast<uint16_t>(y - outer.y);
        }
        return;
}

void WindowManager::release(uint16_t x, uint16_t y) {
        if (_grabId != 0) {
                if (_dragId == _grabId) (void)dropLocked(); // moved: put there
                _grabId = 0;
        }
        if (_closeId != 0) {
                const uint8_t id = _closeId;
                _closeId = 0;
                auto [z, err] = _stack.layerOf(id);
                if (err.isOk() && windowAt(x, y) == id &&
                    WindowFrame::closeBox(_stack.rectAt(z)).contains(Rect{x, y, 1, 1}))
                        (void)closeLocked(id);
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
        return _surface != nullptr && _cursorShown ? cursorArea() : Rect();
}

// The pointer's rectangle on the screen, widened to 4-pixel columns.
Rect WindowManager::cursorArea() const {
        const Rect     screen = _stack.screen();
        const uint32_t left = _pointerX & ~(Align - 1u);
        uint32_t       right = (uint32_t(_pointerX) + CursorWidth + Align - 1u) & ~(Align - 1u);
        uint32_t       bottom = uint32_t(_pointerY) + CursorHeight;
        if (right > screen.width) right = screen.width;
        if (bottom > screen.height) bottom = screen.height;
        return Rect{static_cast<uint16_t>(left), _pointerY, static_cast<uint16_t>(right - left),
                    static_cast<uint16_t>(bottom - _pointerY)};
}

namespace {

        // The arrow: 'X' its edge, 'o' its fill, '.' what shows through.
        constexpr const char *CursorShape[WindowManager::CursorHeight] = {
                "X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......",
                "XooooX......", "XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..",
                "XoooooooooX.", "XooooooXXXXX", "XoooXooX....", "XooXXooX....", "XoX..XooX...",
                "XX...XooX...", "X.....XooX..", "......XooX..", ".......XX...",
        };

        struct CursorJob {
                        uint16_t column; // the area's first column, from the tip's
                        uint16_t width;
                        uint16_t rows;
                        uint32_t edge;
                        uint32_t fill;
        };

} // namespace

// The arrow over what @p pixels holds (the cursor area, from its 4-pixel column).
void WindowManager::drawCursorPixels(void *arg, uint8_t *pixels, size_t stride) {
        const auto *job = static_cast<const CursorJob *>(arg);
        for (uint16_t r = 0; r < job->rows; ++r) {
                const char *shape = CursorShape[r];
                uint8_t    *row = pixels + size_t(r) * stride;
                for (uint16_t c = 0; c < job->width; ++c) {
                        const int sc = int(c) - job->column; // the shape's column
                        if (sc < 0 || sc >= CursorWidth || shape[sc] == '.') continue;
                        const uint32_t rgb = shape[sc] == 'X' ? job->edge : job->fill;
                        uint8_t       *p = row + size_t(c) * Surface::BytesPerPixel;
                        p[0] = static_cast<uint8_t>(rgb);
                        p[1] = static_cast<uint8_t>(rgb >> 8);
                        p[2] = static_cast<uint8_t>(rgb >> 16);
                }
        }
        return;
}

// The pointer over everything drawn this round (drawing it again where it was is harmless).
void WindowManager::drawCursor() {
        _cursorMoved = false;
        if (!_cursorShown) return;
        const Rect area = cursorArea();
        CursorJob  job;
        job.column = static_cast<uint16_t>(_pointerX - area.x);
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
void WindowManager::round() {
        const uint32_t requests = _requests.load(std::memory_order_acquire);
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
        const uint8_t  id = _stack.idAt(z);
        const Window  &w = _windows[id - 1];
        const Rect     outer = _stack.rectAt(z);
        const bool     active = z == 0;
        const uint32_t frame = _palette.color(active ? Role::ActiveFrame : Role::InactiveFrame);
        const uint32_t text = _palette.color(active ? Role::ActiveTitle : Role::InactiveTitle);
        const auto     bit = static_cast<uint16_t>(1u << (id - 1));

        if (!pieces.isEmpty()) {
                Painter chrome(*_surface, pieces, pieces, 0, 0, _tools, DrawTimeoutMs);
                Rect    edges[3];
                WindowFrame::edges(outer, edges);
                const Rect parts[4] = {WindowFrame::titleBar(outer), edges[0], edges[1], edges[2]};
                (void)chrome.fill(parts, frame);
                const Rect box = WindowFrame::closeBox(outer);
                (void)chrome.fill(box, _palette.color(Role::CloseBox));
                (void)chrome.fill(Rect{uint16_t(box.x + Align), uint16_t(box.y + Align), Align, Align},
                                  _palette.color(Role::CloseMark));
                const Rect title = WindowFrame::title(outer);
                (void)chrome.text(title.x, title.y, StringView(w.title), Fonts::unscii16, text, frame, title.width);
                count(chrome);
        }

        // The client area's damage; its clip, the damage or (an update) all of it that shows.
        const Rect client = WindowFrame::client(outer);
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
void WindowManager::changed(uint8_t oldTop) {
        const uint8_t top = _stack.count() > 0 ? _stack.idAt(0) : 0;
        if (top != oldTop) {
                for (const uint8_t id : {oldTop, top}) {
                        auto [z, err] = _stack.layerOf(id);
                        if (err.isError()) continue;
                        const Rect outer = _stack.rectAt(z);
                        Rect       edges[3];
                        WindowFrame::edges(outer, edges);
                        (void)_stack.invalidate(id, WindowFrame::titleBar(outer));
                        for (const Rect &e : edges) (void)_stack.invalidate(id, e);
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
        r.x = static_cast<uint16_t>(r.x & ~(Align - 1u));
        return r;
}

Result<uint8_t> WindowManager::open(const WindowConfig &config) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return makeError<uint8_t>(Error::InvalidState);
        const Rect r = placed(config.rect);
        if (r.width < WindowFrame::MinWidth || r.height < WindowFrame::MinHeight)
                return makeError<uint8_t>(Error::Invalid);
        const uint8_t oldTop = _stack.count() > 0 ? _stack.idAt(0) : 0;
        auto [id, err] = _stack.open(r);
        if (err.isError()) return makeError<uint8_t>(err);
        Window &w = _windows[id - 1];
        w = Window();
        if (config.title != nullptr) {
                std::strncpy(w.title, config.title, MaxTitle);
                w.title[MaxTitle] = '\0';
        }
        w.redraw = config.redraw;
        w.closed = config.closed;
        w.key = config.key;
        w.arg = config.arg;
        changed(oldTop);
        return makeResult(id);
}

Error WindowManager::close(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return closeLocked(id);
}

Error WindowManager::closeLocked(uint8_t id) {
        const uint8_t oldTop = _stack.count() > 0 ? _stack.idAt(0) : 0;
        const Error   err = _stack.close(id);
        if (err.isError()) return err;
        const Window w = _windows[id - 1];
        _windows[id - 1] = Window();
        if (w.closed != nullptr) w.closed(w.arg);
        if (_grabId == id) _grabId = 0;
        if (_closeId == id) _closeId = 0;
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
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return err;
        const Rect at = _stack.rectAt(z);
        err = _stack.move(id, placed(Rect{x, y, at.width, at.height}));
        if (err.isOk()) changed(_stack.idAt(0));
        return err;
}

Error WindowManager::raise(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        return raiseLocked(id);
}

Error WindowManager::raiseLocked(uint8_t id) {
        const uint8_t oldTop = _stack.count() > 0 ? _stack.idAt(0) : 0;
        const Error   err = _stack.raise(id);
        if (err.isOk()) changed(oldTop);
        return err;
}

Error WindowManager::invalidate(uint8_t id, const Rect &rect) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_surface == nullptr) return Error::InvalidState;
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return err;
        const Rect client = WindowFrame::client(_stack.rectAt(z));
        const Rect local{0, 0, client.width, client.height};
        const Rect r = rect.intersected(local);
        if (r.isEmpty()) return Error::Ok;
        (void)_stack.invalidate(id, Rect{uint16_t(client.x + r.x), uint16_t(client.y + r.y), r.width, r.height});
        changed(_stack.idAt(0));
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
        } else {
                _stack.invalidateScreen(_stack.screen());
        }
        _palette = palette;
        changed(_stack.count() > 0 ? _stack.idAt(0) : 0);
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
        const Rect at = _stack.rectAt(z);
        const Rect to = placed(Rect{x, y, at.width, at.height});
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
        changed(_stack.idAt(0));
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
        const uint8_t oldTop = _stack.idAt(0);
        if (_outlineShown) {
                Rect sides[4];
                WindowFrame::outline(_outline, sides);
                for (const Rect &s : sides) _stack.invalidateScreen(s);
        }
        _dragId = 0;
        _outlineShown = false;
        Error err = _stack.raise(id);
        if (err.isOk()) err = _stack.move(id, _outline);
        changed(oldTop);
        return err;
}

size_t WindowManager::list(Span<Info> out) {
        Mutex::Locker lock(_lock, WaitForever);
        size_t        n = 0;
        for (size_t z = 0; z < _stack.count() && n < out.size(); ++z, ++n) {
                Info &info = out[n];
                info.id = _stack.idAt(z);
                info.rect = _stack.rectAt(z);
                std::memcpy(info.title, _windows[info.id - 1].title, sizeof(info.title));
                info.focused = z == 0;
        }
        return n;
}

Result<Rect> WindowManager::clientRect(uint8_t id) {
        Mutex::Locker lock(_lock, WaitForever);
        auto [z, err] = _stack.layerOf(id);
        if (err.isError()) return makeError<Rect>(err);
        return makeResult(WindowFrame::client(_stack.rectAt(z)));
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
