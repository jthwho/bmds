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

#include <cstdio>
#include <cstring>
#include <new>
#include <esp_cache.h>
#include <esp_memory_utils.h>
#include <bmds/blitchain.h>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/desktopentry.h>
#include <bmds/display.h>
#include <bmds/image.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/painter.h>
#include <bmds/pads.h>
#include <bmds/pngdecoder.h>
#include <bmds/ppa.h>
#include <bmds/programsurface.h>
#include <bmds/scanout.h>
#include <bmds/spinlock.h>
#include <bmds/surface.h>
#include <bmds/sysconf.h>
#include <bmds/videomodes.h>
#include "desktopmenu.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t     StripRows = 8;     // full screen's strip: rows of the screen's width
        constexpr uint16_t     DamageColumns = 8; // damage across on these (PIE's 16 bytes in RGB565)
        constexpr uint16_t     ScreenWidth = 1280; // the desktop's mode (devplan/display.md V5)
        constexpr uint16_t     ScreenHeight = 720;
        constexpr unsigned int SwitchTimeoutMs = 1000; // a scan switch rebuilds the link: ~62 ms
        constexpr unsigned int DrawTimeoutMs = 100;

        Display *display0() {
                auto [d, err] = Display::display(0);
                return err.isOk() && d->isRunning() ? d : nullptr;
        }

        // Off by default: the LT8912B doesn't hold VGA's 31.5 kHz modes steadily (games.md §20a).
        // wm/controls: the command a program's window menu runs for its Controls... (with its id).
        BMDS_SYSCONF(wmControls, "wm/controls", Config::Type::String, "controls",
                     "The command a game's window menu runs for Controls... (the application's id added)", 0, 128,
                     nullptr, nullptr)

        BMDS_SYSCONF(fullScreenModes, "display/fullscreen-modes", Config::Type::Bool, "false",
                     "Show a full-screen picture in a mode of the monitor's it fills exactly (Doom's 320x200 in "
                     "640x400 at 70 Hz), the monitor resyncing as it starts and ends; false: scaled on the "
                     "desktop's mode. Experimental: this board's HDMI bridge glitches in those modes",
                     0, 0, nullptr, nullptr)

        bool fullScreenModes() {
                if (!SysConf::isReady()) return false;
                auto [on, err] = SysConf::getBool(bmdsConf_fullScreenModes.key);
                return err.isOk() && on;
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

Error ProgramSurface::start(const Config &requested) {
        Config config = requested;
        if ((config.flags & Maximized) != 0 && config.width == 0 && config.height == 0) {
                // The maximized window's client area, at scale 1 (devplan/luckyduck.md §15).
                auto [wm, werr] = WindowManager::manager(0);
                if (werr.isError() || wm == nullptr) return Error::NotSupported;
                const Rect    work = wm->workArea();
                const uint8_t deco = WindowFrame::Standard;
                const int32_t w = int32_t(work.width) - 2 * WindowFrame::sideWidth(deco);
                const int32_t h = int32_t(work.height) - WindowFrame::topHeight(deco) - WindowFrame::sideWidth(deco);
                config.width = uint16_t((w > ScreenWidth ? ScreenWidth : w > 4 ? w : 4) & ~3);
                config.height = uint16_t(h > ScreenHeight ? ScreenHeight : h > 1 ? h : 1);
        }
        if (config.width == 0 || config.width % 4 != 0 || config.height == 0 || config.width > ScreenWidth ||
            config.height > ScreenHeight || config.buffers < 2 || config.buffers > MaxBuffers ||
            (config.flags & ~(FullScreen | Whole | Fast | Maximized)) != 0 ||
            (config.aspectX == 0) != (config.aspectY == 0) ||
            (config.format != Format::L8 && config.format != Format::Rgb565 && config.format != Format::Bgr24))
                return Error::Invalid;
        _config = config;
        _config.title = StringView();
        const MemSpace space = (config.flags & Fast) != 0 ? MemSpace::InternalDma : MemSpace::PsramDma;
        Error          err = makeBuffers(config.width, config.height, config.format, config.buffers, space);
        if (err.isError()) return err;
        if (config.format == Format::L8) {
                _palette = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, 256 * sizeof(uint32_t)));
                if (_palette == nullptr) return Error::NoMem;
                for (uint32_t i = 0; i < 256; ++i) _palette[i] = i * 0x010101u; // gray until set
        }
        Task::Config tc;
        tc.name = "surface";
        tc.stackBytes = 4096;
        tc.priority = 4; // above programs, below the window manager
        tc.core = 1;     // the display's core (the loader's DisplayCore); the window manager's is 0
        err = _task.start(tc, taskEntry, this);
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
        wc.pad = padEntry;
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
        if ((config.flags & FullScreen) != 0)
                ask(Enter);
        else if ((config.flags & Maximized) != 0)
                (void)wm->maximize(id);
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
        closeWindow(); // no event callback runs after it
        if (ProgramControls *controls = _controls.load(std::memory_order_acquire); controls != nullptr) {
                controls->clearTarget(this);
                controls->unref();
        }
        Memory::free(_palette); // the buffers: SwapChain's
        Memory::free(_strip);
        Memory::free(_triple);
        Memory::free(const_cast<uint32_t *>(_icon.pixels)); // the window is closed: nothing shows them
        Memory::free(const_cast<uint32_t *>(_largeIcon.pixels));
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

// ---- Presents

Result<uint32_t> ProgramSurface::present(uint8_t index, Span<const Rect> damage) {
        if (index >= _config.buffers) return makeError<uint32_t>(Error::Invalid);
        bool all;
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_states[index] != State::Program) return makeError<uint32_t>(Error::Invalid);
                all = _paletteChanged; // every pixel's color may have changed
                _paletteChanged = false;
        }
        const Damage changed = damageOf(damage, all);
        writeBack(index); // the program's writes out of the cache for the DMA (the lock not held)
        bool   freed = false, full = false;
        Rect   redraw[Damage::Max]; // a window's: the changes scaled into its client area
        size_t redraws = 0;
        {
                Mutex::Locker locker(_lock, WaitForever);
                freed = takePresentLocked(index);
                full = _fullScreen;
                if (full) {
                        // Added to what the presenter hasn't converted yet (presents it skips included).
                        _dirty = true;
                        for (const Rect &r : changed.span()) _pending.add(r);
                } else {
                        uint16_t   scale = 1;
                        const Rect pic = pictureRect(_clientWidth, _clientHeight, scale);
                        for (const Rect &r : changed.span())
                                redraw[redraws++] = Rect{uint16_t(pic.x + r.x * scale), uint16_t(pic.y + r.y * scale),
                                                         uint16_t(r.width * scale), uint16_t(r.height * scale)};
                }
        }
        if (freed) _freed.release();
        if (full) {
                _work.release();
        } else if (_wm != nullptr && _window != 0) {
                (void)_wm->invalidate(_window, Span<const Rect>(redraw, redraws));
        }
        return makeResult(frames() + 1);
}

// A present's damage on DamageColumns, merged (@p all, or none given: the whole picture).
ProgramSurface::Damage ProgramSurface::damageOf(Span<const Rect> damage, bool all) const {
        Damage d;
        if (all || damage.size() == 0) {
                d.add(Rect{0, 0, _config.width, _config.height});
                return d;
        }
        for (const Rect &r : damage) {
                Rect a = r.alignedX(DamageColumns);
                if (a.right() > _config.width) a.width = uint16_t(_config.width - a.x); // the width: 4s
                d.add(a);
        }
        return d;
}

void ProgramSurface::Damage::add(const Rect &rect) {
        if (rect.isEmpty()) return;
        // Merged with each one it meets or touches whose bounds with it add little: merging on touch
        // alone chains, one rectangle ending up around everything.
        const auto meets = [](const Rect &a, const Rect &b) {
                return a.x <= b.right() && b.x <= a.right() && a.y <= b.bottom() && b.y <= a.bottom();
        };
        Rect u = rect;
        for (size_t i = 0; i < count;) {
                const Rect &e = rects[i];
                const Rect  overlap = u.intersected(e);
                if (meets(u, e) && u.united(e).area() * 4 <= (u.area() + e.area() - overlap.area()) * 5) {
                        u = u.united(e);
                        rects[i] = rects[--count];
                        i = 0;
                } else {
                        ++i;
                }
        }
        if (count < Max) {
                rects[count++] = u;
                return;
        }
        // Full: the two (the new one among them) whose bounds add the least area merged, in their
        // place even where that overlaps others (only drawn twice).
        Rect all[Max + 1];
        for (size_t i = 0; i < Max; ++i) all[i] = rects[i];
        all[Max] = u;
        size_t  pa = 0, pb = 1;
        int64_t best = INT64_MAX;
        for (size_t a = 0; a <= Max; ++a) {
                for (size_t b = a + 1; b <= Max; ++b) {
                        const int64_t added = int64_t(all[a].united(all[b]).area()) - all[a].area() - all[b].area();
                        if (added < best) best = added, pa = a, pb = b;
                }
        }
        count = 0;
        for (size_t k = 0; k <= Max; ++k)
                if (k != pa && k != pb) rects[count++] = all[k];
        rects[count++] = all[pa].united(all[pb]);
        return;
}


Error ProgramSurface::setPalette(size_t first, Span<const uint32_t> rgb) {
        if (_palette == nullptr) return Error::NotSupported;
        if (first > 256 || rgb.size() > 256 - first) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        for (size_t i = 0; i < rgb.size(); ++i) _palette[first + i] = rgb[i] & 0xffffffu;
        if (rgb.size() > 0) {
                _paletteChanged = true;
                _tripleStale.store(true, std::memory_order_release);
                _clutStale.store(true, std::memory_order_release);
        }
        return Error::Ok;
}

Error ProgramSurface::setTitle(StringView title) {
        if (_wm == nullptr || _window == 0) return Error::InvalidState;
        return _wm->setTitle(_window, title);
}

Error ProgramSurface::setPointerMode(PointerMode mode) {
        if (mode != PointerMode::Normal && mode != PointerMode::Hidden && mode != PointerMode::Relative)
                return Error::Invalid;
        if (_wm == nullptr || _window == 0) return Error::InvalidState;
        // Relative's arrow shows while another window is focused: the lock hides it while held.
        Error err = _wm->setPointer(_window, mode == PointerMode::Hidden ? WindowManager::Pointer::None
                                                                         : WindowManager::Pointer::Arrow);
        if (err.isOk()) err = _wm->setPointerLock(_window, mode == PointerMode::Relative);
        _pointerMode.store(uint8_t(mode), std::memory_order_release);
        _arrowMoved.store(true, std::memory_order_release); // full screen: shown or not, as asked
        _work.release();
        return err;
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
        i.width = _config.width;
        i.height = _config.height;
        if (full) {
                const Layout l = _layout;
                i.scaleX16 = uint16_t(uint32_t(l.width) * 16 / _config.width);
                i.scaleY16 = uint16_t(uint32_t(l.screenHeight) * 16 / l.rows);
                i.area = Rect{l.x, uint16_t(uint32_t(l.top) * l.screenHeight / l.rows), l.width,
                              uint16_t(uint32_t(_config.height) * l.screenHeight / l.rows)};
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

// @p count pixels of a row from @p src across by @p scale into @p out (B, G, R; RGB565 stays
// RGB565, which the 2D-DMA widens).
void ProgramSurface::expandRow(const uint8_t *src, uint16_t count, uint8_t *out, uint16_t scale) const {
        switch (_config.format) {
                case Format::L8: Pixels::expandIndexed(src, count, _palette, out, scale); break;
                case Format::Rgb565:
                        Pixels::repeat16(reinterpret_cast<const uint16_t *>(src), count,
                                         reinterpret_cast<uint16_t *>(out), scale);
                        break;
                default: Pixels::repeat24(src, count, out, scale); break;
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

// @p clip of the picture (at @p pic, @p scale times its size) from @p buf: its rows scaled across
// into the window manager's strip, each shown `scale` times down by the 2D-DMA, a band at a time.
void ProgramSurface::drawScaled(Painter &painter, const uint8_t *buf, const Rect &pic, uint16_t scale,
                                const Rect &clip) {
        if (clip.isEmpty()) return;
        uint16_t c0 = uint16_t((clip.x - pic.x) / scale & ~(DamageColumns - 1u));
        uint16_t c1 = uint16_t((clip.right() - pic.x + scale - 1) / scale);
        c1 = uint16_t((c1 + DamageColumns - 1) & ~(DamageColumns - 1u));
        if (c1 > _config.width) c1 = _config.width;
        Band           b{this, nullptr, scale, 0, c0, uint16_t(c1 - c0)};
        const uint16_t perStrip = painter.stripRows();
        const uint16_t first = uint16_t((clip.y - pic.y) / scale);
        const uint16_t end = uint16_t((clip.bottom() - pic.y + scale - 1) / scale);
        for (uint16_t sy = first; sy < end; sy = uint16_t(sy + perStrip)) {
                b.rows = uint16_t(end - sy < perStrip ? end - sy : perStrip);
                b.src = buf + size_t(sy) * _stride;
                if (painter.rows(pic.x + c0 * scale, pic.y + sy * scale, uint16_t(b.cols * scale), b.rows, scale,
                                 _config.format == Format::Rgb565, makeBand, &b)
                            .isError())
                        break;
        }
        return;
}

// The next piece of a band: its source columns from @p column / scale on, as many whole fours of
// them as fit @p room, each row expanded across.
uint16_t ProgramSurface::makeBand(void *arg, uint8_t *strip, size_t stride, uint16_t column, uint16_t room) {
        const Band    &b = *static_cast<const Band *>(arg);
        const uint16_t at = uint16_t(column / b.scale);
        uint16_t       n = uint16_t((room / b.scale) & ~3u);
        if (n > b.cols - at) n = uint16_t(b.cols - at);
        if (n == 0) return 0;
        const size_t bpp = bytesPerPixel(b.self->_config.format);
        for (uint16_t r = 0; r < b.rows; ++r)
                b.self->expandRow(b.src + size_t(r) * b.self->_stride + (b.c0 + at) * bpp, n,
                                  strip + size_t(r) * stride, b.scale);
        return uint16_t(n * b.scale);
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
                index = beginReadLocked();
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
                        // Each rectangle of the clip on its own (their bounds would expand and write
                        // what didn't change, and each band once a rectangle).
                        for (size_t i = 0; i < painter.clipCount(); ++i)
                                drawScaled(painter, buf, pic, scale, painter.clipAt(i).intersected(pic));
                }
        }
        release(index);
        return;
}

// ---- The window's icons

namespace {

        // @p png (at most @p side pixels each way) into @p icon, its pixels allocated.
        Error decodeIcon(ByteSpan png, uint16_t side, Icon &icon) {
                if (png.isEmpty()) return Error::Ok;
                auto [size, err] = Png::size(png);
                if (err.isError() || size.width == 0 || size.height == 0 || size.width > side || size.height > side)
                        return Error::Invalid;
                const size_t pixels = size_t(size.width) * size.height;
                auto        *mem = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, pixels * sizeof(uint32_t)));
                if (mem == nullptr) return Error::NoMem;
                auto [got, derr] = Png::decode(png, Span<uint32_t>(mem, pixels), MemSpace::Psram);
                if (derr.isError()) {
                        Memory::free(mem);
                        return derr;
                }
                icon = Icon{got.width, got.height, mem};
                return Error::Ok;
        }

} // namespace

Error ProgramSurface::setIcons(ByteSpan small, ByteSpan large) {
        if (_icon.pixels != nullptr || _largeIcon.pixels != nullptr) return Error::InvalidState; // once
        Error err = decodeIcon(small, 16, _icon);
        if (err.isOk()) err = decodeIcon(large, 32, _largeIcon);
        if (err.isError()) return err;
        if (_wm != nullptr && _window != 0) {
                if (_icon.isValid()) (void)_wm->setIcon(_window, &_icon);
                if (_largeIcon.isValid()) (void)_wm->setLargeIcon(_window, &_largeIcon);
        }
        return Error::Ok;
}

// ---- Full screen (the presenter task)

// The picture on the screen: whole multiples both ways (BMDS_SURFACE_WHOLE: rows repeated a
// whole number of times, black rows above and below), else the screen's height filled (rows
// repeated as the scan's mapping falls) and across the picture's shape (its aspect, or square
// pixels): a whole multiple when one is within 3% of it, else that width exactly (columns
// repeated as rows are: games.md §22); centered across. In a mode of the monitor's (@p own),
// the picture fills it by whole factors (VideoModes::fullScreen()).
ProgramSurface::Layout ProgramSurface::layOut(uint16_t screenWidth, uint16_t screenHeight, bool own) const {
        Layout         l;
        l.screenWidth = screenWidth;
        l.screenHeight = screenHeight;
        const uint16_t w = _config.width, h = _config.height;
        uint16_t       most = uint16_t(screenWidth / w);
        if (most > MaxScale) most = MaxScale;
        if (most < 1) most = 1;
        if (own) {
                l.scaleX = uint16_t(screenWidth / w);
                l.rows = h;
                l.top = 0;
        } else if ((_config.flags & Whole) != 0) {
                uint16_t s = uint16_t(screenHeight / h);
                if (s > most) s = most;
                if (s < 1) s = 1;
                while (s > 1 && screenHeight % s != 0) --s;
                l.scaleX = s;
                l.rows = uint16_t(screenHeight / s);
                l.top = uint16_t((l.rows - h) / 2);
        } else {
                // The width that keeps the shape at the screen's height, in whole multiples.
                const uint32_t want = _config.aspectX != 0 ? uint32_t(screenHeight) * _config.aspectX / _config.aspectY
                                                           : uint32_t(screenHeight) * w / h;
                uint32_t       s = (want + w / 2) / w;
                if (s < 1) s = 1;
                if (s > most) s = most;
                l.rows = h;
                l.top = 0;
                const uint32_t off = s * w > want ? s * w - want : want - s * w;
                if (off * 32 > want && want <= screenWidth) { // no whole multiple near it: as wide as it is
                        l.scaleX = 0;
                        l.width = uint16_t(want & ~3u);
                        l.x = uint16_t(((screenWidth - l.width) / 2) & ~3u);
                        return l;
                }
                l.scaleX = uint16_t(s);
        }
        l.width = uint16_t(w * l.scaleX);
        l.x = uint16_t(((screenWidth - l.width) / 2) & ~3u);
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
        // A mode of the monitor's the picture fills exactly, if allowed and there is one; else, and
        // if that fails, scaled on the desktop's.
        VideoTiming mode = d->timing();
        bool        own = false;
        if (fullScreenModes()) {
                auto [edid, eerr] = d->monitor();
                if (eerr.isOk()) {
                        auto [t, terr] = VideoModes::fullScreen(_config.width, _config.height, _config.aspectX,
                                                                _config.aspectY, edid);
                        if (terr.isOk()) {
                                mode = t;
                                own = true;
                        }
                }
        }
        Error err = enterFullScreen(mode, own);
        if (err.isError() && own) {
                bmdsWarn("ProgramSurface: full screen in %ux%u: %s; scaled instead", unsigned(mode.hActive),
                         unsigned(mode.vActive), err.desc());
                err = enterFullScreen(d->timing(), false);
        }
        return err;
}

Error ProgramSurface::enterFullScreen(const VideoTiming &mode, bool own) {
        Display *d = display0();
        if (d == nullptr) return Error::InvalidState;
        _layout = layOut(mode.hActive, mode.vActive, own);
        const int64_t t0 = Clock::monotonicNs();
        bmdsInfo("ProgramSurface: full screen: %ux%u rows, %u across%s", unsigned(mode.hActive), unsigned(_layout.rows),
                 unsigned(_layout.width), own ? ", the monitor's mode" : "");
        // The picture's rows go through a strip the CPU fills, in two halves: one filled while the
        // 2D-DMA copies the other. Only while full screen: a window draws in the window manager's.
        _stripBytes = size_t(ScreenWidth) * Surface::BytesPerPixel * StripRows;
        _strip = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, _stripBytes, 64));
        // L8 three times across (Doom's, Quake's): expandTriple()'s table, 8 KB, a quarter faster
        // than looking the palette up (devplan/bench.md §19); without one if internal RAM is short.
        if (_strip != nullptr && _config.format == Format::L8 && _layout.scaleX == 3) {
                _triple = static_cast<uint32_t *>(
                        Memory::alloc(MemSpace::Internal, Pixels::TripleTableWords * sizeof(uint32_t)));
                _tripleStale.store(true, std::memory_order_release);
        }
        void *memory = _strip != nullptr ? Memory::alloc(MemSpace::Psram, sizeof(Surface)) : nullptr;
        if (memory == nullptr) {
                Memory::free(_strip);
                Memory::free(_triple);
                _triple = nullptr;
                _strip = nullptr;
                return Error::NoMem;
        }
        _fs = ::new (memory) Surface();
        Error err = _fs->start(mode.hActive, _layout.rows, Surface::Buffering::Double); // black
        if (err.isOk()) {
                Mutex::Locker locker(_lock, WaitForever);
                _fullScreen = true;
                _dirty = _latest >= 0;
                _pending.count = 0;
                _pending.add(Rect{0, 0, _config.width, _config.height}); // the new surface is black
        }
        const int64_t t1 = Clock::monotonicNs();
        if (err.isOk())
                err = own ? d->show(*_fs, mode, 0, 0, SwitchTimeoutMs) : d->scanout().show(*_fs, SwitchTimeoutMs);
        const int64_t t2 = Clock::monotonicNs();
        if (err.isOk()) err = _wm->setFullScreen(_window, true);
        if (err.isOk()) {
                _arrow = _wm->arrowPicture();
                _underRect = Rect();
                uint16_t px = 0, py = 0;
                _wm->pointerPosition(px, py);
                const int32_t mx = int32_t(px) * _layout.screenWidth / ScreenWidth;
                const int32_t my = int32_t(py) * _layout.screenHeight / ScreenHeight;
                _arrowAt.store(mx | my << 16, std::memory_order_release);
                _arrowMoved.store(true, std::memory_order_release);
        }
        bmdsInfo("ProgramSurface: full screen: %s; surface %lld us, scan %lld us, window manager %lld us", err.desc(),
                 (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
                 (long long)((Clock::monotonicNs() - t2) / 1000));
        if (err.isError()) {
                {
                        Mutex::Locker locker(_lock, WaitForever);
                        _fullScreen = false;
                }
                freeFullScreen(d);
                Memory::free(_strip);
                Memory::free(_triple);
                _triple = nullptr;
                _strip = nullptr;
                return err;
        }
        startPpa();
        pushPresented();
        return Error::Ok;
}

// L8 at a whole scale: the PPA's CLUT expands the indices (repeated across by the CPU) into the
// scan surface, the CPU's palette lookups gone (devplan/demo.md §8a). Without it (the PPA busy
// elsewhere, no memory), the CPU expands as before.
void ProgramSurface::startPpa() {
        if (_config.format != Format::L8 || _layout.scaleX == 0 || _ppa != nullptr) return;
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Ppa));
        if (memory == nullptr) return;
        _ppa = ::new (memory) Ppa();
        const Error err = _ppa->start();
        if (err.isError()) {
                bmdsInfo("ProgramSurface: the PPA isn't free (%s): L8 expanded by the CPU", err.desc());
                stopPpa();
                return;
        }
        _clutStale.store(true, std::memory_order_release);
        Memory::free(_triple); // the CPU's table for 3x: not needed
        _triple = nullptr;
        // The rows fetched into the strip by the 2D-DMA, so the CPU repeats them from internal RAM
        // rather than wait on PSRAM (devplan/demo.md §8c). Without it, the CPU reads them itself.
        if (_layout.scaleX >= 2) {
                void *fetch = Memory::alloc(MemSpace::Psram, sizeof(BlitChain));
                if (fetch != nullptr) {
                        _fetch = ::new (fetch) BlitChain();
                        const Error ferr = _fetch->start(BlitChain::Conversion::None, 1);
                        if (ferr.isError()) {
                                bmdsInfo("ProgramSurface: no 2D-DMA for the rows (%s): read by the CPU", ferr.desc());
                                _fetch->~BlitChain();
                                Memory::free(_fetch);
                                _fetch = nullptr;
                        }
                }
        }
        return;
}

void ProgramSurface::stopPpa() {
        if (_fetch != nullptr) {
                _fetch->~BlitChain(); // waits for a job under way
                Memory::free(_fetch);
                _fetch = nullptr;
        }
        if (_ppa == nullptr) return;
        _ppa->~Ppa(); // waits for a job under way
        Memory::free(_ppa);
        _ppa = nullptr;
        return;
}

// The full-screen surface freed once the scanout holds nothing of it: the desktop shown first if
// it is the one sent. If the scan can't be moved off it, it is kept (and logged): a surface freed
// under the scanout is read by its interrupt (a panic, 2026-11-06).
void ProgramSurface::freeFullScreen(Display *d) {
        if (d != nullptr && d->scanout().forget(*_fs).isError()) {
                (void)d->showDesktop(SwitchTimeoutMs);
                if (d->scanout().forget(*_fs).isError()) {
                        bmdsWarn("ProgramSurface: the scan stays on a full-screen surface: kept, not freed");
                        _fs = nullptr;
                        return;
                }
        }
        _fs->~Surface();
        Memory::free(_fs);
        _fs = nullptr;
        return;
}

void ProgramSurface::leaveFullScreen() {
        bmdsInfo("ProgramSurface: leaving full screen");
        Display *d = display0();
        {
                Mutex::Locker locker(_lock, WaitForever);
                _fullScreen = false;
                _dirty = false;
        }
        if (_wm != nullptr && _window != 0) (void)_wm->setFullScreen(_window, false);
        if (_fs != nullptr) {
                (void)_fs->wait(DrawTimeoutMs);
                freeFullScreen(d);
        }
        _underRect = Rect();
        _arrowAt.store(-1, std::memory_order_release);
        stopPpa();
        Memory::free(_strip); // the last copy from it waited for above
        _strip = nullptr;
        Memory::free(_triple);
        _triple = nullptr;
        pushPresented();
        if (_wm != nullptr && _window != 0) (void)_wm->update(_window);
        return;
}

// The newest present into the scan surface's back buffer, rows scaled across in strips: one half
// of the strip filled by the CPU while the 2D-DMA copies the other; then presented (swapped at
// the next frame's start).
void ProgramSurface::convert() {
        int8_t index = -1;
        Damage changed;
        bool   dirty;
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (!_fullScreen || _fs == nullptr) return;
                dirty = _dirty;
                if (dirty) {
                        _dirty = false;
                        changed = _pending;
                        _pending.count = 0;
                        index = beginReadLocked();
                }
        }
        const bool moved = _arrowMoved.exchange(false, std::memory_order_acq_rel);
        if (!dirty && !moved) return;
        restoreUnder(); // before anything is drawn: nothing drawn copies the arrow
        if (index >= 0) {
                const Layout l = _layout;
                const bool   wide = _config.format == Format::Rgb565;
                const size_t bpp = bytesPerPixel(_config.format);
                const size_t half = _stripBytes / 2;
                const Rect   clip[1] = {_fs->bounds()};
                // Where the changes land; the rest of the back buffer is the last present's (the
                // scan surface copies it forward).
                Rect on[Damage::Max];
                for (size_t i = 0; i < changed.count; ++i) {
                        const Rect &r = changed.rects[i];
                        if (l.scaleX == 0) { // the columns showing it, on 4-pixel columns
                                const uint32_t w = _config.width;
                                const uint32_t x0 = (uint32_t(r.x) * l.width / w) & ~3u;
                                const uint32_t x1 = ((uint32_t(r.right()) * l.width + w - 1) / w + 3) & ~3u;
                                on[i] = Rect{uint16_t(l.x + x0), uint16_t(l.top + r.y),
                                             uint16_t((x1 < l.width ? x1 : l.width) - x0), r.height};
                        } else {
                                on[i] = Rect{uint16_t(l.x + r.x * l.scaleX), uint16_t(l.top + r.y),
                                             uint16_t(r.width * l.scaleX), r.height};
                        }
                }
                if (_ppa != nullptr && convertWithPpa(index, changed.rects, on, changed.count)) {
                        drawArrow();
                        (void)_fs->present(DrawTimeoutMs);
                        release(index);
                        return;
                }
                (void)_fs->willDraw(Span<const Rect>(on, changed.count), DrawTimeoutMs);
                const uint8_t *buf = _buffers[index];
                int            k = 0;
                Error          err;
                if (_triple != nullptr && _tripleStale.exchange(false, std::memory_order_acq_rel)) {
                        Mutex::Locker locker(_lock, WaitForever); // the palette as one
                        Pixels::makeTripleTable(_palette, _triple);
                }
                for (size_t i = 0; i < changed.count && err.isOk(); ++i) {
                        const Rect    &r = changed.rects[i];
                        const size_t   rowBytes = size_t(on[i].width) * (wide ? 2 : Surface::BytesPerPixel);
                        const uint16_t perHalf = uint16_t(half / rowBytes > 0 ? half / rowBytes : 1);
                        for (uint16_t y = 0; y < r.height; y = uint16_t(y + perHalf), k ^= 1) {
                                const uint16_t rows = uint16_t(r.height - y < perHalf ? r.height - y : perHalf);
                                uint8_t       *strip = _strip + size_t(k) * half;
                                // This half's last copy started two strips ago; startBlit() waited for it
                                // with the last.
                                for (uint16_t row = 0; row < rows; ++row) {
                                        const uint8_t *src = buf + size_t(r.y + y + row) * _stride;
                                        uint8_t       *dst = strip + size_t(row) * rowBytes;
                                        if (l.scaleX == 0)
                                                Pixels::stretch(src, unsigned(bpp), _palette, uint32_t(on[i].x - l.x),
                                                                on[i].width, _config.width, l.width, dst);
                                        else if (_triple != nullptr)
                                                Pixels::expandTriple(src + r.x, r.width, _triple, dst);
                                        else
                                                expandRow(src + r.x * bpp, r.width, dst, l.scaleX);
                                }
                                err = _fs->startBlit(Rect{on[i].x, uint16_t(on[i].y + y), on[i].width, rows}, strip,
                                                     rowBytes, Span<const Rect>(clip, 1),
                                                     wide ? Surface::Source::Rgb565 : Surface::Source::Bgr24,
                                                     DrawTimeoutMs);
                                if (err.isError()) break;
                        }
                }
                (void)_fs->wait(DrawTimeoutMs);
        }
        drawArrow();
        (void)_fs->present(DrawTimeoutMs);
        if (dirty) release(index);
        return;
}

// The newest present's @p count changed rectangles (@p changed, landing at @p on) by the PPA: each
// row's indices repeated across into one half of the strip by the CPU while the CLUT expands the
// other half into the scan surface's back buffer. @return false if it couldn't start (then the CPU
// converts; nothing was written).
bool ProgramSurface::convertWithPpa(int8_t index, const Rect *changed, const Rect *on, size_t count) {
        if (_clutStale.exchange(false, std::memory_order_acq_rel)) {
                uint32_t palette[Ppa::PaletteSize];
                {
                        Mutex::Locker locker(_lock, WaitForever); // the palette as one
                        std::memcpy(palette, _palette, sizeof(palette));
                }
                if (_ppa->wait(DrawTimeoutMs).isError()) return false;
                _ppa->setPalette(palette, 0, Ppa::PaletteSize);
        }
        auto [back, err] = _fs->beginWrite(Span<const Rect>(on, count), DrawTimeoutMs);
        if (err.isError()) {
                _clutStale.store(true, std::memory_order_release);
                return false;
        }
        const Layout       l = _layout;
        const Ppa::Picture screen{back, _fs->width(), _fs->height(), Ppa::Format::Rgb888};
        const size_t       half = _stripBytes / 2;
        const uint8_t     *buf = _buffers[index];
        // The jobs: each rectangle in runs of the rows a half holds.
        struct Job {
                        size_t   i;    // the rectangle
                        uint16_t y;    // its first row
                        uint16_t rows; // 0: none (past the last)
        };
        const auto perHalfOf = [&](size_t i) {
                return uint16_t(half / on[i].width > 0 ? half / on[i].width : 1);
        };
        const auto jobFrom = [&](size_t i, uint16_t y) {
                for (; i < count; ++i, y = 0) {
                        if (y >= changed[i].height) continue;
                        const uint16_t perHalf = perHalfOf(i);
                        return Job{i, y, uint16_t(changed[i].height - y < perHalf ? changed[i].height - y : perHalf)};
                }
                return Job{count, 0, 0};
        };
        // Fetched by the 2D-DMA when a rectangle's rows are whole 16-byte steps (PIE's, in place):
        // into the end of the half's rows, repeated forward from there (the reads stay ahead of the
        // writes); else read by the CPU from PSRAM.
        const auto fetched = [&](const Job &j) {
                return _fetch != nullptr && l.scaleX >= 2 && (changed[j.i].width & 15) == 0;
        };
        const auto tailOf = [&](const Job &j, int half_) {
                return _strip + size_t(half_) * half + size_t(j.rows) * (on[j.i].width - changed[j.i].width);
        };
        const BlitChain::Picture frame{buf, uint16_t(_stride), _config.height, 1};
        const auto               startFetch = [&](const Job &j, int half_) {
                const Rect              &r = changed[j.i];
                const BlitChain::Picture to{tailOf(j, half_), r.width, j.rows, 1};
                // The lines the rows land in out of the CPU's cache, so it reads what the 2D-DMA
                // wrote (rounded out to whole lines: the half is all written back, nothing lost).
                const uintptr_t first = reinterpret_cast<uintptr_t>(to.data) & ~uintptr_t(63);
                const uintptr_t last =
                        (reinterpret_cast<uintptr_t>(to.data) + size_t(j.rows) * r.width + 63) & ~uintptr_t(63);
                (void)esp_cache_msync(reinterpret_cast<void *>(first), last - first, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                Error e = _fetch->add(frame, r.x, uint16_t(r.y + j.y), to, 0, 0, r.width, j.rows);
                if (e.isOk()) e = _fetch->submit();
                return e;
        };
        int  k = 0;
        Job  j = jobFrom(0, 0);
        bool pending = false; // a fetch under way for j
        if (j.rows != 0 && fetched(j)) {
                err = startFetch(j, k);
                pending = err.isOk();
        }
        while (j.rows != 0 && err.isOk()) {
                const Rect    &r = changed[j.i];
                const uint16_t width = on[j.i].width; // L8: a byte a pixel
                uint8_t       *strip = _strip + size_t(k) * half;
                // This half's last job ended when the other half's started (one job at a time).
                if (pending) {
                        // The PPA's last job first: the fetch, started with it and far shorter, is
                        // done by then, so this task blocks once a job, not twice (each block is two
                        // switches; the PPA idles only while the rows are repeated, ~15 µs).
                        err = _ppa->wait(DrawTimeoutMs);
                        if (err.isOk()) err = _fetch->wait(DrawTimeoutMs);
                        if (err.isError()) break;
                        const uint8_t *in = tailOf(j, k);
                        for (uint16_t row = 0; row < j.rows; ++row)
                                Pixels::repeat8(in + size_t(row) * r.width, r.width, strip + size_t(row) * width,
                                                l.scaleX);
                } else {
                        for (uint16_t row = 0; row < j.rows; ++row)
                                Pixels::repeat8(buf + size_t(r.y + j.y + row) * _stride + r.x, r.width,
                                                strip + size_t(row) * width, l.scaleX);
                }
                // Written back for the PPA.
                (void)esp_cache_msync(strip, size_t(j.rows) * width,
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                const Ppa::Picture from{strip, width, j.rows, Ppa::Format::L8};
                err = _ppa->expand(from, Rect{0, 0, width, j.rows}, screen, on[j.i].x, uint16_t(on[j.i].y + j.y), false,
                                   DrawTimeoutMs);
                // The other half is free now (expand() waited for its job): the next rows into it.
                const Job next = jobFrom(j.i, uint16_t(j.y + j.rows));
                k ^= 1;
                pending = false;
                if (err.isOk() && next.rows != 0 && fetched(next)) {
                        err = startFetch(next, k);
                        pending = err.isOk();
                }
                j = next;
        }
        if (_fetch != nullptr) (void)_fetch->wait(DrawTimeoutMs); // nothing writes the strip after this
        const Error done = _ppa->wait(DrawTimeoutMs); // before anything else touches the back buffer
        if (err.isError() || done.isError())
                bmdsWarn("ProgramSurface: the PPA's expansion: %s", (err.isError() ? err : done).desc());
        return true;
}

namespace {

        struct ArrowJob {
                        const WindowManager::CursorPicture *arrow;
                        int32_t                             column; // the picture's first column, from the area's
                        uint16_t                            width;  // the area's
                        uint16_t                            rows;
                        uint16_t                            firstRow; // the area's first row, on the scan surface
                        uint32_t                            screenRows, scanRows; // the scan's mapping
                        int32_t                             pictureTop; // the picture's first row, on the screen
                        uint8_t                            *under;
        };

        struct UnderJob {
                        const uint8_t *under;
                        uint16_t       width;
                        uint16_t       rows;
        };

        // The arrow over the area's pixels, a scan row taking the picture's row its first screen
        // row shows (the scan repeats rows: the arrow keeps its size on the screen).
        void drawArrowPixels(void *arg, uint8_t *pixels, size_t stride) {
                const auto  *job = static_cast<const ArrowJob *>(arg);
                const auto  &p = *job->arrow;
                const size_t bytes = size_t(job->width) * Surface::BytesPerPixel;
                for (uint16_t r = 0; r < job->rows; ++r)
                        std::memcpy(job->under + r * bytes, pixels + size_t(r) * stride, bytes);
                for (uint16_t r = 0; r < job->rows; ++r) {
                        const int32_t screenRow = int32_t(
                                (uint32_t(job->firstRow + r) * job->screenRows + job->scanRows - 1) / job->scanRows);
                        const int32_t sr = screenRow - job->pictureTop;
                        if (sr < 0 || sr >= p.height) continue;
                        const char *shape = p.rows[sr];
                        uint8_t    *row = pixels + size_t(r) * stride;
                        for (uint16_t c = 0; c < job->width; ++c) {
                                const int32_t sc = int32_t(c) - job->column;
                                if (sc < 0 || sc >= p.width || shape[sc] == '.') continue;
                                const uint32_t rgb = shape[sc] == 'X' ? p.edge : p.fill;
                                uint8_t       *px = row + size_t(c) * Surface::BytesPerPixel;
                                px[0] = uint8_t(rgb);
                                px[1] = uint8_t(rgb >> 8);
                                px[2] = uint8_t(rgb >> 16);
                        }
                }
                return;
        }

        void restoreArrowPixels(void *arg, uint8_t *pixels, size_t stride) {
                const auto  *job = static_cast<const UnderJob *>(arg);
                const size_t bytes = size_t(job->width) * Surface::BytesPerPixel;
                for (uint16_t r = 0; r < job->rows; ++r)
                        std::memcpy(pixels + size_t(r) * stride, job->under + r * bytes, bytes);
                return;
        }

} // namespace

void ProgramSurface::restoreUnder() {
        if (_underRect.isEmpty() || _fs == nullptr) return;
        UnderJob job{_under, _underRect.width, _underRect.height};
        (void)_fs->patch(_underRect, restoreArrowPixels, &job, DrawTimeoutMs);
        _underRect = Rect();
        return;
}

// The arrow, where the pointer is, unless the program hides it (hidden, or held for mouse look).
void ProgramSurface::drawArrow() {
        const int32_t at = _arrowAt.load(std::memory_order_acquire);
        if (_fs == nullptr || at < 0 || _arrow.rows == nullptr ||
            PointerMode(_pointerMode.load(std::memory_order_acquire)) != PointerMode::Normal)
                return;
        const Layout  l = _layout;
        const Rect    bounds = _fs->bounds();
        const int32_t x = (at & 0xffff) - _arrow.hotX, y = (at >> 16) - _arrow.hotY;
        constexpr int Align = 4; // whole 4-pixel columns, as the surface's copies like
        int32_t       left = x < 0 ? 0 : x & ~(Align - 1);
        int32_t       right = (x + _arrow.width + Align - 1) & ~(Align - 1);
        // The scan rows showing the picture's screen rows.
        int32_t first = y < 0 ? 0 : int32_t(uint32_t(y) * l.rows / l.screenHeight);
        int32_t last = int32_t((uint32_t(y + _arrow.height > 0 ? y + _arrow.height : 0) * l.rows + l.screenHeight - 1) /
                               l.screenHeight);
        if (right > bounds.width) right = bounds.width;
        if (last > bounds.height) last = bounds.height;
        if (right <= left || last <= first) return;
        const Rect area{uint16_t(left), uint16_t(first), uint16_t(right - left), uint16_t(last - first)};
        if (size_t(area.width) * area.height * Surface::BytesPerPixel > sizeof(_under)) return;
        ArrowJob job{&_arrow, x - left, area.width, area.height, area.y, l.screenHeight, l.rows, y, _under};
        if (_fs->patch(area, drawArrowPixels, &job, DrawTimeoutMs).isOk()) _underRect = area;
        return;
}

void ProgramSurface::fullScreenEntry(void *arg, bool on) {
        static_cast<ProgramSurface *>(arg)->ask(on ? Enter : Leave);
        return;
}

// ---- Events (the window manager's task, its lock held: no calls into it)

// Queued for the program. A mouse mustn't crowd out keys (a key's release lost is a key held for
// good): a move with the buttons as they were joins the move queued last, if that is the newest
// event; and a full queue drops its oldest move or wheel, a key, action or pad event only if it
// holds nothing else (logged).
void ProgramSurface::push(const Event &event) {
        uint32_t lost = 0;
        {
                Mutex::Locker locker(_queueLock, WaitForever);
                if (event.type == uint32_t(EventType::Pointer) && _count > 0) {
                        Event &last = _queue[(_head + _count - 1) % QueueEvents];
                        if (last.type == event.type && last.data[2] == event.data[2]) {
                                auto sum = [](uint32_t a, uint32_t b, int shift) {
                                        const int32_t v = int32_t(int16_t(a >> shift)) + int32_t(int16_t(b >> shift));
                                        return uint32_t(uint16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v)) << shift;
                                };
                                last.data[0] = event.data[0];
                                last.data[1] = event.data[1];
                                last.data[3] =
                                        sum(last.data[3], event.data[3], 0) | sum(last.data[3], event.data[3], 16);
                                last.timeNs = event.timeNs;
                                return; // the reader, woken for the first, takes it
                        }
                }
                if (_count == QueueEvents) {
                        size_t victim = 0; // the oldest move or wheel, else the oldest
                        for (size_t i = 0; i < _count; ++i) {
                                const uint32_t t = _queue[(_head + i) % QueueEvents].type;
                                if (t == uint32_t(EventType::Pointer) || t == uint32_t(EventType::Wheel)) {
                                        victim = i;
                                        break;
                                }
                                if (i + 1 == _count) lost = ++_lostEvents;
                        }
                        for (size_t i = victim; i + 1 < _count; ++i)
                                _queue[(_head + i) % QueueEvents] = _queue[(_head + i + 1) % QueueEvents];
                        --_count;
                }
                _queue[(_head + _count) % QueueEvents] = event;
                ++_count;
        }
        if (lost == 1 || (lost != 0 && lost % 100 == 0))
                bmdsWarn("ProgramSurface: events lost, the program not reading (%lu so far)", (unsigned long)lost);
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
                        const uint32_t down16 = uint32_t(_layout.screenHeight) * 16 / _layout.rows;
                        e.data[0] = 1;
                        e.data[1] = uint32_t(_layout.width) * 16 / _config.width | down16 << 16;
                        e.data[2] = uint32_t(_layout.width) |
                                    (uint32_t(_config.height) * _layout.screenHeight / _layout.rows) << 16;
                } else {
                        e.data[0] = 0;
                        e.data[1] = uint32_t(scale * 16) | uint32_t(scale * 16) << 16;
                        e.data[2] = uint32_t(pic.width) | uint32_t(pic.height) << 16;
                }
        }
        push(e);
        return;
}

void ProgramSurface::setControls(ProgramControls *controls) {
        if (controls == nullptr || _controls.load(std::memory_order_acquire) != nullptr) return;
        controls->ref();
        _controls.store(controls, std::memory_order_release);
        bool active;
        {
                Mutex::Locker locker(_lock, WaitForever);
                active = _active;
        }
        if (active) controls->setTarget(actionEntry, this);
        // Its window menu offers the Controls window on its application (devplan/controls.md §7d).
        controls->appId(_appId);
        if (_appId[0] != '\0' && _wm != nullptr && _window != 0)
                (void)_wm->setWindowItem(_window, "C&ontrols...", controlsChosen, this);
        return;
}

// The window menu's Controls...: the command wm/controls names, with the application's id.
void ProgramSurface::controlsChosen(void *arg) {
        auto  *self = static_cast<ProgramSurface *>(arg);
        String command;
        if (SysConf::get("wm/controls", command).isError() || command.isEmpty()) return;
        char      line[DesktopEntry::MaxExec + 1];
        const int n = std::snprintf(line, sizeof(line), "%s %s", command.cStr(), self->_appId);
        if (n <= 0 || size_t(n) >= sizeof(line)) return;
        const Error err = runDesktopCommand(StringView(line, size_t(n)));
        if (err.isError()) bmdsWarn("ProgramSurface: %s: %s", line, err.desc());
        return;
}

// A matched action's event (the ABI's BMDS_EVENT_ACTION).
void ProgramSurface::actionEntry(void *arg, const Controls::Output &output) {
        Event e;
        e.type = uint32_t(EventType::Action);
        e.flags = uint32_t(output.type);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = output.id;
        e.data[1] = uint32_t(output.value);
        e.data[2] = output.source;
        static_cast<ProgramSurface *>(arg)->push(e);
        return;
}

void ProgramSurface::keyEntry(void *arg, const KeyEvent &event) {
        auto *self = static_cast<ProgramSurface *>(arg);
        // The program's actions first: a match isn't delivered raw.
        if (ProgramControls *controls = self->_controls.load(std::memory_order_acquire);
            controls != nullptr && controls->key(0x00070000u | event.usage, Chord::modifiersOf(event.modifiers),
                                                 event.action, actionEntry, self))
                return;
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

void ProgramSurface::padEntry(void *arg, const PadEvent &event) {
        auto *self = static_cast<ProgramSurface *>(arg);
        if (ProgramControls *controls = self->_controls.load(std::memory_order_acquire);
            controls != nullptr && controls->pad(event, actionEntry, self))
                return;
        Event e;
        e.type = uint32_t(EventType::Pad);
        e.flags = uint32_t(event.type);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = event.pad;
        e.data[1] = event.type == PadEvent::Type::Attached ? uint32_t(event.value) : event.index;
        e.data[2] = event.type == PadEvent::Type::Attached ? 0 : uint32_t(event.value);
        self->push(e);
        return;
}

// The focus came (each pad attached, and what is down or off centre on it) or went (what is down
// comes up, sticks to centre): nothing stays held that the program can't see let go.
void ProgramSurface::padsSettled(bool active) {
        Pads::Info   pads[Pads::MaxPads];
        const size_t n = Pads::list(Span<Pads::Info>(pads, Pads::MaxPads));
        for (size_t i = 0; i < n; ++i) {
                PadState state;
                RawPad   raw;
                if (Pads::state(pads[i].number, state, raw).isError()) continue;
                if (active)
                        padEntry(this, PadEvent{PadEvent::Type::Attached, pads[i].number, 0,
                                                int32_t(uint32_t(pads[i].vendor) << 16 | pads[i].product)});
                for (uint8_t b = 0; b < PadState::ButtonCount; ++b)
                        if (state.isDown(PadState::Button(b)))
                                padEntry(this, PadEvent{PadEvent::Type::Button, pads[i].number, b, active ? 1 : 0});
                for (uint8_t a = 0; a < PadState::AxisCount; ++a)
                        if (state.axes[a] != 0)
                                padEntry(this,
                                         PadEvent{PadEvent::Type::Axis, pads[i].number, a, active ? state.axes[a] : 0});
        }
        return;
}

void ProgramSurface::mouseEntry(void *arg, const WindowManager::MouseEvent &event) {
        auto   *self = static_cast<ProgramSurface *>(arg);
        int32_t x, y;
        if (event.type == WindowManager::MouseEvent::Type::Leave) {
                // Gone (BMDS_POINTER_GONE): the program unlights what was under it.
                Event e;
                e.type = uint32_t(EventType::Pointer);
                e.timeNs = Clock::monotonicNs();
                e.data[0] = uint32_t(int32_t(PointerGone));
                e.data[1] = uint32_t(int32_t(PointerGone));
                e.data[2] = event.buttons;
                e.data[3] = 0;
                self->push(e);
                return;
        }
        {
                Mutex::Locker locker(self->_lock, WaitForever);
                if (self->_fullScreen) {
                        // The desktop's coordinates into the mode's, then the line's row (the scan's
                        // mapping), less the black ones.
                        const Layout  l = self->_layout;
                        const int32_t mx = int32_t(event.x) * l.screenWidth / ScreenWidth;
                        const int32_t my = int32_t(event.y) * l.screenHeight / ScreenHeight;
                        const int32_t dx = mx - l.x;
                        if (self->_arrowAt.exchange(mx | my << 16, std::memory_order_acq_rel) != (mx | my << 16)) {
                                self->_arrowMoved.store(true, std::memory_order_release);
                                self->_work.release(); // the presenter moves the arrow
                        }
                        y = my * l.rows / l.screenHeight - l.top;
                        // The column's input pixel (Pixels::stretch()'s mapping), rounded down.
                        const int32_t w = self->_config.width, across = l.width;
                        x = dx >= 0 ? dx * w / across : -((-dx * w + across - 1) / across);
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
        e.data[3] = uint32_t(uint16_t(event.dx)) | uint32_t(uint16_t(event.dy)) << 16;
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
        ProgramControls *controls = self->_controls.load(std::memory_order_acquire);
        if (!active) {
                self->padsSettled(false);
                if (controls != nullptr) { // keys held let go too, and a set change told nowhere
                        controls->releaseAll(actionEntry, self);
                        controls->clearTarget(self);
                }
        } else if (controls != nullptr) {
                controls->setTarget(actionEntry, self);
        }
        Event e;
        e.type = uint32_t(EventType::Active);
        e.timeNs = Clock::monotonicNs();
        e.data[0] = active ? 1 : 0;
        self->push(e);
        if (active) self->padsSettled(true);
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
