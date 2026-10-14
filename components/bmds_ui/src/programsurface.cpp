/**
 * @file      programsurface.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's surface (devplan/games.md §3, §10c, §11): a swap chain of the program's buffers
 * shown in a window (scaled by whole multiples) or full screen (the scan's own surface, rows
 * repeated by the scan); its events queued for read().
 */

#include <cstring>
#include <new>
#include <esp_cache.h>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/display.h>
#include <bmds/image.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/painter.h>
#include <bmds/programsurface.h>
#include <bmds/scanout.h>
#include <bmds/surface.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       BufferAlign = 128; // the L2 cache line; the 2D-DMA's PSRAM alignment
        constexpr uint16_t     StripRows = 8;     // scaled rows in the strip
        constexpr size_t       WriteBackChunk = 64 * 1024;
        constexpr uint16_t     ScreenWidth = 1280; // the mode's (devplan/display.md V5)
        constexpr uint16_t     ScreenHeight = 720;
        constexpr unsigned int SwitchTimeoutMs = 1000; // a scan switch rebuilds the link: ~62 ms
        constexpr unsigned int DrawTimeoutMs = 100;
        constexpr unsigned int WaitSliceMs = 20; // a frame wait looks again this often (the scanned surface may change)

        size_t bytesPerPixel(ProgramSurface::Format format) {
                switch (format) {
                        case ProgramSurface::Format::L8: return 1;
                        case ProgramSurface::Format::Rgb565: return 2;
                        default: return 3;
                }
        }

        unsigned int remainingMs(const Deadline &deadline) {
                const int64_t us = deadline.remainingUs();
                if (us == Deadline::Forever) return WaitForever;
                return us <= 0 ? 1 : static_cast<unsigned int>((us + 999) / 1000);
        }

        Display *display0() {
                auto [d, err] = Display::display(0);
                return err.isOk() && d->isRunning() ? d : nullptr;
        }

} // namespace

Result<FileHandle::Ptr> ProgramSurface::open(const Config &config) {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(ProgramSurface));
        if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *surface = ::new (memory) ProgramSurface();
        Error err = surface->start(config);
        if (err.isOk()) {
                auto [handle, herr] = FileHandle::open(*surface, FileHandle::Read);
                if (herr.isOk()) return makeResult(std::move(handle));
                err = herr;
        }
        surface->~ProgramSurface();
        Memory::free(surface);
        return makeError<FileHandle::Ptr>(err);
}

Error ProgramSurface::start(const Config &config) {
        if (config.width == 0 || config.width % 4 != 0 || config.height == 0 || config.width > ScreenWidth ||
            config.height > ScreenHeight || config.buffers < 2 || config.buffers > MaxBuffers ||
            (config.flags & ~(FullScreen | Whole | Fast)) != 0 || (config.aspectX == 0) != (config.aspectY == 0) ||
            (config.format != Format::L8 && config.format != Format::Rgb565 && config.format != Format::Bgr24))
                return Error::Invalid;
        _config = config;
        _config.title = StringView();
        _stride = size_t(config.width) * bytesPerPixel(config.format);
        _bufferBytes = (_stride * config.height + BufferAlign - 1) / BufferAlign * BufferAlign;
        const MemSpace space = (config.flags & Fast) != 0 ? MemSpace::InternalDma : MemSpace::PsramDma;
        for (uint8_t i = 0; i < config.buffers; ++i) {
                _buffers[i] = static_cast<uint8_t *>(Memory::allocAligned(space, _bufferBytes, BufferAlign));
                if (_buffers[i] == nullptr) return Error::NoMem;
                std::memset(_buffers[i], 0, _bufferBytes);
                (void)esp_cache_msync(_buffers[i], _bufferBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                _states[i] = State::Free;
        }
        if (config.format == Format::L8) {
                _palette = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, 256 * sizeof(uint32_t)));
                if (_palette == nullptr) return Error::NoMem;
                for (uint32_t i = 0; i < 256; ++i) _palette[i] = i * 0x010101u; // gray until set
        }
        // Scaled rows (and every L8 picture) go through a strip the CPU fills: full screen in two
        // halves, one filled while the 2D-DMA copies the other.
        _stripBytes = size_t(ScreenWidth) * Surface::BytesPerPixel * StripRows;
        _strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, _stripBytes, 64));
        if (_strip == nullptr) return Error::NoMem;

        Task::Config tc;
        tc.name = "surface";
        tc.stackBytes = 4096;
        tc.priority = 4; // above programs, below the window manager
        tc.core = 1;     // the window manager's is 0
        Error err = _task.start(tc, taskEntry, this);
        if (err.isError()) return err;
        _taskStarted = true;

        auto [wm, werr] = WindowManager::manager(0);
        if (werr.isError() || wm == nullptr) return Error::NotSupported;
        _wm = wm;
        auto [client, cerr] = wm->connect(goneEntry, this);
        if (cerr.isError()) return cerr;
        _client = client;
        // A window at the largest whole scale the work area takes (at most 2 to begin with).
        const Rect    work = wm->workArea();
        const uint8_t deco = WindowFrame::Standard;
        uint16_t      scale = 2;
        while (scale > 1 && (WindowFrame::outerWidth(uint16_t(config.width * scale), deco) > work.width ||
                             WindowFrame::outerHeight(uint16_t(config.height * scale), deco) > work.height))
                --scale;
        WindowManager::WindowConfig wc;
        wc.rect = Rect{0, 0, WindowFrame::outerWidth(uint16_t(config.width * scale), deco),
                       WindowFrame::outerHeight(uint16_t(config.height * scale), deco)};
        char         title[WindowManager::MaxTitle + 1] = {};
        const size_t titleBytes =
                config.title.size() < WindowManager::MaxTitle ? config.title.size() : WindowManager::MaxTitle;
        std::memcpy(title, config.title.data(), titleBytes);
        wc.title = title;
        wc.redraw = redrawEntry;
        wc.arg = this;
        wc.closed = closedEntry;
        wc.key = keyEntry;
        wc.resized = resizedEntry;
        wc.decorations = deco;
        wc.widthStep = config.width;
        wc.heightStep = config.height;
        wc.wheel = wheelEntry;
        wc.client = client;
        wc.kind = "surface";
        wc.mouse = mouseEntry;
        wc.cascade = true;
        wc.activated = activatedEntry;
        wc.fullScreen = fullScreenEntry;
        {
                Mutex::Locker locker(_lock, WaitForever);
                _clientWidth = uint16_t(config.width * scale);
                _clientHeight = uint16_t(config.height * scale);
        }
        auto [id, oerr] = wm->open(wc);
        if (oerr.isError()) return oerr;
        _window = id;
        pushPresented();
        // The program's suggestion (the user's setting per application comes with apps.md).
        if ((config.flags & FullScreen) != 0) ask(Enter);
        return Error::Ok;
}

ProgramSurface::~ProgramSurface() {
        if (_taskStarted) {
                // The task leaves full screen (the scan back on surface 0) before it ends.
                _stopping.store(true, std::memory_order_release);
                _work.release();
                (void)_taskDone.acquire(WaitForever);
                (void)_task.stop();
        }
        closeWindow();
        for (uint8_t *&b : _buffers) {
                Memory::free(b);
                b = nullptr;
        }
        Memory::free(_palette);
        Memory::free(_strip);
}

void ProgramSurface::closeWindow() {
        if (_wm == nullptr) return;
        // After close() returns no callback runs again (the calls wait for a round being drawn).
        if (_window != 0) (void)_wm->close(_window);
        _window = 0;
        if (_client != 0) _wm->disconnect(_client);
        _client = 0;
        _wm = nullptr;
        return;
}

void ProgramSurface::handleClosed(uint8_t) {
        this->~ProgramSurface();
        Memory::free(this);
        return;
}

uint32_t ProgramSurface::frames() {
        Display *d = display0();
        return d != nullptr ? d->scanout().stats().frames : 0;
}

// ---- The swap chain

Result<uint8_t> ProgramSurface::tryAcquire() {
        Mutex::Locker locker(_lock, WaitForever);
        for (uint8_t i = 0; i < _config.buffers; ++i) {
                if (_states[i] != State::Free) continue;
                _states[i] = State::Program;
                return makeResult(i);
        }
        return makeError<uint8_t>(Error::WouldBlock);
}

Result<uint8_t> ProgramSurface::acquire(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        for (;;) {
                auto r = tryAcquire();
                if (r.error() != Error::WouldBlock) return r;
                const Error err = Interrupt::waitCurrent(_freed, deadline);
                if (err.isError()) return makeError<uint8_t>(err);
        }
}

Result<uint32_t> ProgramSurface::present(uint8_t index, Span<const Rect> damage) {
        if (index >= _config.buffers) return makeError<uint32_t>(Error::Invalid);
        (void)damage; // whole pictures for now (rows by damage: devplan/games.md §10b''')
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_states[index] != State::Program) return makeError<uint32_t>(Error::Invalid);
        }
        // The program's writes out of the cache for the DMA, in pieces (the lock not held).
        const size_t bytes = _stride * _config.height;
        for (size_t off = 0; off < bytes; off += WriteBackChunk)
                (void)esp_cache_msync(_buffers[index] + off,
                                      bytes - off < WriteBackChunk ? bytes - off : WriteBackChunk,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        bool freed = false, full = false;
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_latest >= 0) {
                        // The one it replaces is free, unless it is being drawn (then when that ends).
                        if (_latest != _reading) {
                                _states[_latest] = State::Free;
                                freed = true;
                        }
                        if (!_latestDrawn) ++_dropped; // replaced before it was ever shown
                }
                _states[index] = State::Latest;
                _latest = int8_t(index);
                _latestDrawn = false;
                ++_presents;
                full = _fullScreen;
                if (full) _dirty = true;
        }
        if (freed) _freed.release();
        if (full) {
                _work.release();
        } else if (_wm != nullptr && _window != 0) {
                (void)_wm->update(_window);
        }
        return makeResult(frames() + 1);
}

// Done reading presented buffer @p index: free if it was replaced meanwhile.
void ProgramSurface::release(int8_t index) {
        bool freed = false;
        {
                Mutex::Locker locker(_lock, WaitForever);
                _reading = -1;
                if (index >= 0 && index != _latest && _states[index] == State::Latest) {
                        _states[index] = State::Free;
                        freed = true;
                }
        }
        if (freed) _freed.release();
        return;
}

Error ProgramSurface::setPalette(size_t first, Span<const uint32_t> rgb) {
        if (_palette == nullptr) return Error::NotSupported;
        if (first > 256 || rgb.size() > 256 - first) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        for (size_t i = 0; i < rgb.size(); ++i) _palette[first + i] = rgb[i] & 0xffffffu;
        return Error::Ok;
}

Error ProgramSurface::setTitle(StringView title) {
        if (_wm == nullptr || _window == 0) return Error::InvalidState;
        return _wm->setTitle(_window, title);
}

Error ProgramSurface::setPointerMode(PointerMode mode) {
        (void)mode; // hidden full screen; a window shows the arrow (relative mode: devplan/games.md §7 step 5)
        return Error::Ok;
}

Error ProgramSurface::waitFrame(uint32_t frame, unsigned int timeoutMs) {
        Display *d = display0();
        if (d == nullptr) return Error::InvalidState;
        const Deadline deadline(timeoutMs);
        for (;;) {
                if (int32_t(frames() - frame) >= 0) return Error::Ok;
                // The surface scanned now (it changes with full screen): its frames, a slice at a time.
                Surface *scanned = d->scanout().surface();
                if (scanned == nullptr) return Error::InvalidState;
                const unsigned int left = remainingMs(deadline);
                const unsigned int slice = left == WaitForever || left > WaitSliceMs ? WaitSliceMs : left;
                const Error        err = scanned->waitFrame(scanned->frames(), slice);
                if (err.isError() && err != Error::Timeout) return err;
                if (err == Error::Timeout && !deadline.isForever() && deadline.remainingUs() <= 0)
                        return Error::Timeout;
        }
}

ProgramSurface::Info ProgramSurface::info() {
        Info     i;
        uint16_t scale = 1;
        bool     full;
        {
                Mutex::Locker locker(_lock, WaitForever);
                i.active = _active;
                full = _fullScreen;
                i.area = pictureRect(_clientWidth, _clientHeight, scale);
                i.presents = _presents;
                i.dropped = _dropped;
        }
        i.fullScreen = full;
        i.shown = full || _window != 0;
        if (full) {
                const Layout l = _layout;
                i.scaleX16 = uint16_t(l.scaleX * 16);
                i.scaleY16 = uint16_t(uint32_t(ScreenHeight) * 16 / l.rows);
                i.area = Rect{l.x, uint16_t(uint32_t(l.top) * ScreenHeight / l.rows), l.width,
                              uint16_t(uint32_t(_config.height) * ScreenHeight / l.rows)};
        } else {
                i.scaleX16 = uint16_t(scale * 16);
                i.scaleY16 = uint16_t(scale * 16);
                // Not under the lock: the window manager's calls wait for a round, whose drawing takes it.
                if (_wm != nullptr && _window != 0) {
                        auto [client, err] = _wm->clientRect(_window);
                        if (err.isOk()) {
                                i.area.x = uint16_t(i.area.x + client.x);
                                i.area.y = uint16_t(i.area.y + client.y);
                        }
                }
        }
        i.frames = frames();
        return i;
}

// ---- Scaling rows

// Row @p src of the picture across by @p scale into @p out (B, G, R; RGB565 stays RGB565, which
// the 2D-DMA widens).
void ProgramSurface::expandRow(const uint8_t *src, uint8_t *out, uint16_t scale) const {
        switch (_config.format) {
                case Format::L8: Pixels::expandIndexed(src, _config.width, _palette, out, scale); break;
                case Format::Rgb565:
                        Pixels::repeat16(reinterpret_cast<const uint16_t *>(src), _config.width,
                                         reinterpret_cast<uint16_t *>(out), scale);
                        break;
                default: Pixels::repeat24(src, _config.width, out, scale); break;
        }
        return;
}

// ---- A window (the window manager's task)

// The picture in a client area of @p width × @p height: the largest whole scale that fits (at
// least 1, at most MaxScale), centered on 4-pixel columns.
Rect ProgramSurface::pictureRect(uint16_t width, uint16_t height, uint16_t &scale) const {
        uint16_t sx = uint16_t(width / _config.width), sy = uint16_t(height / _config.height);
        scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
        if (scale > MaxScale) scale = MaxScale;
        const uint16_t w = uint16_t(_config.width * scale), h = uint16_t(_config.height * scale);
        const uint16_t x = w < width ? uint16_t(((width - w) / 2) & ~3u) : 0;
        const uint16_t y = h < height ? uint16_t((height - h) / 2) : 0;
        return Rect{x, y, w, h};
}

void ProgramSurface::redrawEntry(void *arg, Painter &painter) {
        static_cast<ProgramSurface *>(arg)->draw(painter);
        return;
}

void ProgramSurface::draw(Painter &painter) {
        int8_t   index;
        uint16_t scale = 1;
        Rect     pic;
        {
                Mutex::Locker locker(_lock, WaitForever);
                index = _latest;
                _reading = index;
                if (index >= 0) _latestDrawn = true; // shown, even if replaced while drawn
                pic = pictureRect(_clientWidth, _clientHeight, scale);
        }
        // The borders around the picture.
        const Rect bounds{0, 0, _clientWidth, _clientHeight};
        Rect       borders[4];
        size_t     n = 0;
        if (pic.y > 0) borders[n++] = Rect{0, 0, bounds.width, pic.y};
        if (pic.bottom() < bounds.height)
                borders[n++] = Rect{0, uint16_t(pic.bottom()), bounds.width, uint16_t(bounds.height - pic.bottom())};
        if (pic.x > 0) borders[n++] = Rect{0, pic.y, pic.x, pic.height};
        if (pic.right() < bounds.width)
                borders[n++] = Rect{uint16_t(pic.right()), pic.y, uint16_t(bounds.width - pic.right()), pic.height};
        if (n > 0) (void)painter.fill(Span<const Rect>(borders, n), 0x000000);
        (void)painter.cover(pic); // drawn whole below: nothing of it copied forward first
        if (index < 0) {
                (void)painter.fill(pic, 0x000000);
        } else {
                const uint8_t *buf = _buffers[index];
                if (scale == 1 && _config.format == Format::Bgr24) {
                        (void)painter.pixels(pic.x, pic.y, buf, _stride, _config.width, _config.height);
                } else if (scale == 1 && _config.format == Format::Rgb565) {
                        (void)painter.pixels565(pic.x, pic.y, buf, _stride, _config.width, _config.height);
                } else {
                        // Rows scaled across into the strip, each written `scale` times down; only the
                        // strips the painter's clip reaches.
                        const Rect     clip = painter.clipBounds();
                        const bool     wide = _config.format == Format::Rgb565;
                        const size_t   rowBytes = size_t(pic.width) * (wide ? 2 : Surface::BytesPerPixel);
                        const uint16_t perStrip = uint16_t(StripRows / scale > 0 ? StripRows / scale : 1);
                        for (uint16_t sy = 0; sy < _config.height; sy = uint16_t(sy + perStrip)) {
                                const uint16_t rows =
                                        uint16_t(_config.height - sy < perStrip ? _config.height - sy : perStrip);
                                const Rect band{pic.x, uint16_t(pic.y + sy * scale), pic.width, uint16_t(rows * scale)};
                                if (!band.intersects(clip)) continue;
                                for (uint16_t r = 0; r < rows; ++r) {
                                        uint8_t *dst = _strip + size_t(r) * scale * rowBytes;
                                        expandRow(buf + size_t(sy + r) * _stride, dst, scale);
                                        for (uint16_t k = 1; k < scale; ++k)
                                                std::memcpy(dst + k * rowBytes, dst, rowBytes);
                                }
                                if (wide) {
                                        (void)painter.pixels565(band.x, band.y, _strip, rowBytes, band.width,
                                                                band.height);
                                } else {
                                        (void)painter.pixels(band.x, band.y, _strip, rowBytes, band.width, band.height);
                                }
                        }
                }
        }
        release(index);
        return;
}

// ---- Full screen (the presenter task)

// The picture on the screen: whole multiples both ways (BMDS_SURFACE_WHOLE: rows repeated a
// whole number of times, black rows above and below), else the screen's height filled (rows
// repeated as the scan's mapping falls) and across the whole multiple nearest the picture's
// shape (its aspect, or square pixels); centered across.
ProgramSurface::Layout ProgramSurface::layOut() const {
        Layout         l;
        const uint16_t w = _config.width, h = _config.height;
        uint16_t       most = uint16_t(ScreenWidth / w);
        if (most > MaxScale) most = MaxScale;
        if (most < 1) most = 1;
        if ((_config.flags & Whole) != 0) {
                uint16_t s = uint16_t(ScreenHeight / h);
                if (s > most) s = most;
                if (s < 1) s = 1;
                while (s > 1 && ScreenHeight % s != 0) --s;
                l.scaleX = s;
                l.rows = uint16_t(ScreenHeight / s);
                l.top = uint16_t((l.rows - h) / 2);
        } else {
                // The width that keeps the shape at the screen's height, in whole multiples.
                const uint32_t want = _config.aspectX != 0 ? uint32_t(ScreenHeight) * _config.aspectX / _config.aspectY
                                                           : uint32_t(ScreenHeight) * w / h;
                uint32_t       s = (want + w / 2) / w;
                if (s < 1) s = 1;
                if (s > most) s = most;
                l.scaleX = uint16_t(s);
                l.rows = h;
                l.top = 0;
        }
        l.width = uint16_t(w * l.scaleX);
        l.x = uint16_t(((ScreenWidth - l.width) / 2) & ~3u);
        return l;
}

void ProgramSurface::ask(Ask what) {
        _ask.store(what, std::memory_order_release);
        _work.release();
        return;
}

void ProgramSurface::taskEntry(void *arg) {
        static_cast<ProgramSurface *>(arg)->run();
        for (;;) Task::sleep(WaitForever); // the destructor stops it
}

void ProgramSurface::run() {
        while (!_stopping.load(std::memory_order_acquire)) {
                (void)_work.acquire(WaitForever);
                if (_stopping.load(std::memory_order_acquire)) break;
                const auto what = static_cast<Ask>(_ask.exchange(None, std::memory_order_acq_rel));
                bool       full;
                {
                        Mutex::Locker locker(_lock, WaitForever);
                        full = _fullScreen;
                }
                if (what == Enter && !full) {
                        const Error err = enterFullScreen();
                        if (err.isError()) bmdsWarn("ProgramSurface: full screen: %s", err.desc());
                } else if (what == Leave && full) {
                        leaveFullScreen();
                }
                convert();
        }
        // Ending: the desktop back on the scan before the surface goes.
        if (_fs != nullptr) leaveFullScreen();
        _taskDone.release();
        return;
}

Error ProgramSurface::enterFullScreen() {
        Display *d = display0();
        if (d == nullptr || _wm == nullptr || _window == 0) return Error::InvalidState;
        _layout = layOut();
        const int64_t t0 = Clock::monotonicNs();
        bmdsInfo("ProgramSurface: full screen: %ux%u rows at %ux across", unsigned(ScreenWidth), unsigned(_layout.rows),
                 unsigned(_layout.scaleX));
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Surface));
        if (memory == nullptr) return Error::NoMem;
        _fs = ::new (memory) Surface();
        Error err = _fs->start(ScreenWidth, _layout.rows, Surface::Buffering::Double); // black
        if (err.isOk()) {
                Mutex::Locker locker(_lock, WaitForever);
                _fullScreen = true;
                _dirty = _latest >= 0;
        }
        const int64_t t1 = Clock::monotonicNs();
        if (err.isOk()) err = d->scanout().show(*_fs, SwitchTimeoutMs);
        const int64_t t2 = Clock::monotonicNs();
        if (err.isOk()) err = _wm->setFullScreen(_window, true);
        bmdsInfo("ProgramSurface: full screen: %s; surface %lld us, scan %lld us, window manager %lld us", err.desc(),
                 (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
                 (long long)((Clock::monotonicNs() - t2) / 1000));
        if (err.isError()) {
                {
                        Mutex::Locker locker(_lock, WaitForever);
                        _fullScreen = false;
                }
                if (d->scanout().surface() == _fs) (void)d->scanout().show(d->surface(), SwitchTimeoutMs);
                _fs->~Surface();
                Memory::free(_fs);
                _fs = nullptr;
                return err;
        }
        pushPresented();
        return Error::Ok;
}

void ProgramSurface::leaveFullScreen() {
        bmdsInfo("ProgramSurface: leaving full screen");
        Display *d = display0();
        {
                Mutex::Locker locker(_lock, WaitForever);
                _fullScreen = false;
                _dirty = false;
        }
        if (d != nullptr && d->scanout().surface() == _fs) (void)d->scanout().show(d->surface(), SwitchTimeoutMs);
        if (_wm != nullptr && _window != 0) (void)_wm->setFullScreen(_window, false);
        if (_fs != nullptr) {
                (void)_fs->wait(DrawTimeoutMs);
                _fs->~Surface();
                Memory::free(_fs);
                _fs = nullptr;
        }
        pushPresented();
        if (_wm != nullptr && _window != 0) (void)_wm->update(_window);
        return;
}

// The newest present into the scan surface's back buffer, rows scaled across in strips: one half
// of the strip filled by the CPU while the 2D-DMA copies the other; then presented (swapped at
// the next frame's start).
void ProgramSurface::convert() {
        int8_t index;
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (!_fullScreen || !_dirty || _fs == nullptr) return;
                _dirty = false;
                index = _latest;
                _reading = index;
                if (index >= 0) _latestDrawn = true;
        }
        if (index >= 0) {
                const Layout   l = _layout;
                const bool     wide = _config.format == Format::Rgb565;
                const size_t   rowBytes = size_t(l.width) * (wide ? 2 : Surface::BytesPerPixel);
                const size_t   half = _stripBytes / 2;
                const uint16_t perHalf = uint16_t(half / rowBytes > 0 ? half / rowBytes : 1);
                const Rect     whole{l.x, l.top, l.width, _config.height};
                const Rect     clip[1] = {_fs->bounds()};
                (void)_fs->willDraw(Span<const Rect>(&whole, 1), DrawTimeoutMs);
                const uint8_t *buf = _buffers[index];
                int            k = 0;
                for (uint16_t y = 0; y < _config.height; y = uint16_t(y + perHalf), k ^= 1) {
                        const uint16_t rows = uint16_t(_config.height - y < perHalf ? _config.height - y : perHalf);
                        uint8_t       *strip = _strip + size_t(k) * half;
                        // This half's last copy started two strips ago; startBlit() waited for it with the last.
                        for (uint16_t r = 0; r < rows; ++r)
                                expandRow(buf + size_t(y + r) * _stride, strip + size_t(r) * rowBytes, l.scaleX);
                        const Error err =
                                _fs->startBlit(Rect{l.x, uint16_t(l.top + y), l.width, rows}, strip, rowBytes,
                                               Span<const Rect>(clip, 1),
                                               wide ? Surface::Source::Rgb565 : Surface::Source::Bgr24, DrawTimeoutMs);
                        if (err.isError()) break;
                }
                (void)_fs->wait(DrawTimeoutMs);
                (void)_fs->present(DrawTimeoutMs);
        }
        release(index);
        return;
}

void ProgramSurface::fullScreenEntry(void *arg, bool on) {
        static_cast<ProgramSurface *>(arg)->ask(on ? Enter : Leave);
        return;
}

// ---- Events (the window manager's task, its lock held: no calls into it)

void ProgramSurface::push(const Event &event) {
        {
                Mutex::Locker locker(_queueLock, WaitForever);
                if (_count == QueueEvents) { // full: the oldest goes
                        _head = (_head + 1) % QueueEvents;
                        --_count;
                }
                _queue[(_head + _count) % QueueEvents] = event;
                ++_count;
        }
        _ready.release();
        _pollers.notify();
        return;
}

void ProgramSurface::pushPresented() {
        Event e;
        e.type = uint32_t(EventType::Presented);
        e.timeNs = Clock::monotonicNs();
        {
                Mutex::Locker locker(_lock, WaitForever);
                uint16_t      scale = 1;
                const Rect    pic = pictureRect(_clientWidth, _clientHeight, scale);
                if (_fullScreen) {
                        const uint32_t down16 = uint32_t(ScreenHeight) * 16 / _layout.rows;
                        e.data[0] = 1;
                        e.data[1] = uint32_t(_layout.scaleX * 16) | down16 << 16;
                        e.data[2] = uint32_t(_layout.width) | (uint32_t(_config.height) * ScreenHeight / _layout.rows)
                                                                      << 16;
                } else {
                        e.data[0] = 0;
                        e.data[1] = uint32_t(scale * 16) | uint32_t(scale * 16) << 16;
                        e.data[2] = uint32_t(pic.width) | uint32_t(pic.height) << 16;
                }
        }
        push(e);
        return;
}

void ProgramSurface::keyEntry(void *arg, const KeyEvent &event) {
        auto *self = static_cast<ProgramSurface *>(arg);
        Event e;
        e.type = uint32_t(EventType::Key);
        e.flags = event.action == KeyEvent::Action::Press    ? KeyDown
                  : event.action == KeyEvent::Action::Repeat ? KeyRepeat
                                                             : KeyUp;
        e.timeNs = Clock::monotonicNs();
        e.data[0] = 0x00070000u | event.usage; // the keyboard page (hotkeys.md §3.1)
        e.data[1] = event.modifiers;
        e.data[2] = event.codepoint;
        self->push(e);
        return;
}

void ProgramSurface::mouseEntry(void *arg, const WindowManager::MouseEvent &event) {
        auto *self = static_cast<ProgramSurface *>(arg);
        if (event.type == WindowManager::MouseEvent::Type::Leave) return;
        int32_t x, y;
        {
                Mutex::Locker locker(self->_lock, WaitForever);
                if (self->_fullScreen) {
                        // The screen's coordinates: the line's row (the scan's mapping), less the black ones.
                        const Layout  l = self->_layout;
                        const int32_t dx = int32_t(event.x) - l.x;
                        y = int32_t(event.y) * l.rows / ScreenHeight - l.top;
                        x = dx >= 0 ? dx / l.scaleX : -((-dx + l.scaleX - 1) / l.scaleX);
                } else {
                        uint16_t   scale = 1;
                        const Rect pic = self->pictureRect(self->_clientWidth, self->_clientHeight, scale);
                        // In the surface's own pixels (outside it while a button is held: negative or past it).
                        const int32_t dx = int32_t(event.x) - pic.x, dy = int32_t(event.y) - pic.y;
                        x = dx >= 0 ? dx / scale : -((-dx + scale - 1) / scale);
                        y = dy >= 0 ? dy / scale : -((-dy + scale - 1) / scale);
                }
        }
        Event e;
        e.type = uint32_t(EventType::Pointer);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = uint32_t(x);
        e.data[1] = uint32_t(y);
        e.data[2] = event.buttons;
        self->push(e);
        return;
}

void ProgramSurface::wheelEntry(void *arg, int notches) {
        Event e;
        e.type = uint32_t(EventType::Wheel);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = uint32_t(int32_t(notches));
        static_cast<ProgramSurface *>(arg)->push(e);
        return;
}

void ProgramSurface::activatedEntry(void *arg, bool active) {
        auto *self = static_cast<ProgramSurface *>(arg);
        bool  full;
        {
                Mutex::Locker locker(self->_lock, WaitForever);
                self->_active = active;
                full = self->_fullScreen;
        }
        if (!active && full) self->ask(Leave); // another window took the keys: the desktop back
        Event e;
        e.type = uint32_t(EventType::Active);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = active ? 1 : 0;
        self->push(e);
        return;
}

void ProgramSurface::resizedEntry(void *arg, uint16_t width, uint16_t height) {
        auto *self = static_cast<ProgramSurface *>(arg);
        {
                Mutex::Locker locker(self->_lock, WaitForever);
                self->_clientWidth = width;
                self->_clientHeight = height;
        }
        self->pushPresented();
        return;
}

// The window is gone (its close button, Alt+F4): the program is told and decides; its presents
// show nowhere until it ends.
void ProgramSurface::closedEntry(void *arg) {
        auto *self = static_cast<ProgramSurface *>(arg);
        self->_window = 0;
        self->ask(Leave);
        Event e;
        e.type = uint32_t(EventType::Close);
        e.timeNs = Clock::monotonicNs();
        self->push(e);
        return;
}

// The window manager stopped: no window, no connection.
void ProgramSurface::goneEntry(void *arg) {
        auto *self = static_cast<ProgramSurface *>(arg);
        self->_window = 0;
        self->_client = 0;
        self->_wm = nullptr;
        self->ask(Leave);
        Event e;
        e.type = uint32_t(EventType::Close);
        e.timeNs = Clock::monotonicNs();
        self->push(e);
        return;
}

// ---- The stream: events

Result<size_t> ProgramSurface::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(Event)) return makeError<size_t>(Error::Invalid);
        const Deadline deadline(timeoutMs);
        for (;;) {
                size_t n = 0;
                {
                        Mutex::Locker locker(_queueLock, WaitForever);
                        while (_count > 0 && n + sizeof(Event) <= buf.size()) {
                                std::memcpy(buf.data() + n, &_queue[_head], sizeof(Event));
                                _head = (_head + 1) % QueueEvents;
                                --_count;
                                n += sizeof(Event);
                        }
                }
                if (n > 0) return makeResult(n);
                const Error err = Interrupt::waitCurrent(_ready, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

Result<size_t> ProgramSurface::write(ByteSpan, unsigned int) {
        return makeError<size_t>(Error::NotSupported);
}

uint8_t ProgramSurface::readiness() {
        Mutex::Locker locker(_queueLock, WaitForever);
        return _count > 0 ? Readable : 0;
}

BMDS_NAMESPACE_END
