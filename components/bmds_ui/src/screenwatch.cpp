/**
 * @file      screenwatch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs watching the screen (devplan/vnc.md §5b, §5c): every present of the scanned surface
 * ORed into each watcher's tiles, and the changed tiles copied out on request.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <esp_cache.h>
#include <bmds/changetiles.h>
#include <bmds/display.h>
#include <bmds/memory.h>
#include <bmds/blitchain.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/screenwatch.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       OwnerBytes = 32;
        constexpr size_t       CacheLine = 128;       // the L2's: the surfaces' buffers are aligned to it
        constexpr size_t       SyncChunk = 64 * 1024; // a cache operation runs with interrupts off
        constexpr unsigned int HoldSettleMs = 50;     // a frame's start, after holding a full screen's swaps

        struct Watcher {
                        bool                used = false;
                        bool                notified = false; // told of changes not yet captured
                        ScreenWatch::Notify notify = nullptr;
                        void               *arg = nullptr;
                        uint32_t            client = 0;       // its GUI client (the tray's mark)
                        const Surface      *source = nullptr; // the surface its tiles are of
                        ChangeTiles         tiles;
                        char                owner[OwnerBytes] = {};
        };

        // In PSRAM: about 550 bytes of tiles each, made with the first watcher, kept.
        constinit Watcher              *watchers = nullptr;
        constinit std::atomic<Mutex *>  lockPtr{nullptr};
        constinit std::atomic<size_t>   active{0};
        constinit Display              *display = nullptr; // display 0, once a watcher was added
        constinit ScreenWatch::Activity activityFn = nullptr;
        constinit void                 *activityArg = nullptr;
        // The desktop's latest present: the buffer holding it, from its hook (the back one then;
        // shown or still waiting for its swap since). Null until a present seen with watchers.
        constinit const Surface *latestSurface = nullptr;
        constinit const uint8_t *latestBuffer = nullptr;

        // The lock, made on first use (whoever loses the race frees theirs).
        Mutex &lock() {
                Mutex *m = lockPtr.load(std::memory_order_acquire);
                if (m != nullptr) return *m;
                void  *mem = Memory::alloc(MemSpace::Internal, sizeof(Mutex));
                auto  *made = ::new (mem) Mutex();
                Mutex *expected = nullptr;
                if (!lockPtr.compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
                        made->~Mutex();
                        Memory::free(mem);
                        return *expected;
                }
                return *made;
        }

        // The surface the scan shows: a program's full screen, or the desktop's.
        const Surface *scanned() {
                if (display == nullptr || !display->isRunning()) return nullptr;
                const Surface *s = display->scanout().surface();
                return s != nullptr ? s : &display->surface();
        }

        // The rows @p s fills on the screen: its own, or in line mode (a program's full screen shorter
        // than the mode) the mode's, its rows repeated as the scan repeats them (screen row k shows
        // the surface's row k * rows / lines). Captures and change tiles are in these rows, so a
        // viewer sees what the monitor shows (Doom at 4:3 was squashed otherwise).
        uint16_t shownRows(const Surface &s) {
                if (display != nullptr && display->isRunning() && display->scanout().surface() == &s) {
                        const uint16_t lines = display->scanout().timing().vActive;
                        if (s.height() < lines) return lines;
                }
                return s.height();
        }

        // Watcher @p w's tiles of @p s: a new source (or size) is all new. Lock held.
        void follow(Watcher &w, const Surface &s) {
                const uint16_t lines = shownRows(s);
                if (w.source == &s && w.tiles.width() == s.width() && w.tiles.height() == lines) return;
                w.source = &s;
                if (!w.tiles.resize(s.width(), lines)) w.tiles = ChangeTiles(); // too big: nothing kept
                return;
        }

        // A surface's changed tiles marked on the screen's rows (line mode: each tile's rows mapped).
        void markMapped(ChangeTiles &t, const uint32_t *tiles, uint16_t rowWords, uint16_t rows, uint16_t height,
                        uint16_t lines) {
                for (uint16_t r = 0; r < rows; ++r) {
                        const uint32_t y0 = uint32_t(r) * ChangeTiles::TileHeight;
                        if (y0 >= height) break;
                        const uint32_t y1 =
                                y0 + ChangeTiles::TileHeight < height ? y0 + ChangeTiles::TileHeight : height;
                        // Screen rows k with k * height / lines in [y0, y1).
                        const uint32_t top = (y0 * lines + height - 1) / height;
                        const uint32_t bottom = (y1 * lines + height - 1) / height;
                        for (uint16_t w = 0; w < rowWords; ++w) {
                                uint32_t bits = tiles[size_t(r) * rowWords + w];
                                while (bits != 0) {
                                        const uint32_t c = uint32_t(w) * 32 + uint32_t(__builtin_ctz(bits));
                                        bits &= bits - 1;
                                        t.mark(Rect{uint16_t(c * ChangeTiles::TileWidth), uint16_t(top),
                                                    ChangeTiles::TileWidth, uint16_t(bottom - top)});
                                }
                        }
                }
                return;
        }

        // Every surface's present: the scanned one's changes to each watcher.
        void presented(void *, const Surface &surface, const uint32_t *tiles, uint16_t rowWords, uint16_t rows) {
                if (active.load(std::memory_order_acquire) == 0 || display == nullptr) return;
                const bool desktop = &surface == &display->surface();
                const bool isScanned = &surface == scanned();
                if (!desktop && !isScanned) return;
                const uint16_t lines = shownRows(surface);
                Mutex::Locker  locker(lock(), WaitForever);
                if (desktop) {
                        latestSurface = &surface;
                        latestBuffer = surface.drawn();
                }
                if (!isScanned) return;
                for (size_t i = 0; i < ScreenWatch::MaxWatchers; ++i) {
                        Watcher &w = watchers[i];
                        if (!w.used) continue;
                        const bool fresh = w.source != &surface || w.tiles.height() != lines;
                        follow(w, surface);
                        if (!fresh) {
                                if (tiles == nullptr) {
                                        w.tiles.markAll();
                                } else if (lines == surface.height()) {
                                        w.tiles.orIn(tiles, rowWords, rows);
                                } else {
                                        markMapped(w.tiles, tiles, rowWords, rows, surface.height(), lines);
                                }
                        }
                        if (!w.notified && !w.tiles.isEmpty()) {
                                w.notified = true;
                                w.notify(w.arg);
                        }
                }
                return;
        }

        // The watchers' number changed: the display and the listener told. Lock held.
        void countChanged() {
                const size_t n = active.load(std::memory_order_acquire);
                if (display != nullptr) display->setViewers(n > 0);
                if (activityFn != nullptr) activityFn(activityArg);
                return;
        }

        // Rows [@p first, @p end) of @p from (@p stride bytes a row) fetched from PSRAM: the CPU never
        // writes a shown buffer, so dropping its lines loses nothing.
        void syncRows(const uint8_t *from, size_t stride, uint32_t first, uint32_t end) {
                const uintptr_t begin = (uintptr_t(from) + size_t(first) * stride) & ~uintptr_t(CacheLine - 1);
                const uintptr_t stop =
                        (uintptr_t(from) + size_t(end) * stride + CacheLine - 1) & ~uintptr_t(CacheLine - 1);
                for (uintptr_t at = begin; at < stop; at += SyncChunk) {
                        const size_t n = stop - at < SyncChunk ? stop - at : SyncChunk;
                        (void)esp_cache_msync(reinterpret_cast<void *>(at), n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                }
                return;
        }

        // The program's buffer, @p bytes of it, for the 2D-DMA to write: its lines written back and
        // dropped before (none evicted onto the DMA's bytes), dropped again after (the CPU then reads
        // what the DMA wrote). Only a buffer and length on cache lines (BMDS_CAPTURE_ALIGN).
        void syncForDma(uint8_t *buffer, size_t bytes, bool after) {
                const int flags = after ? ESP_CACHE_MSYNC_FLAG_DIR_M2C
                                        : ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE;
                for (size_t at = 0; at < bytes; at += SyncChunk) {
                        const size_t n = bytes - at < SyncChunk ? bytes - at : SyncChunk;
                        (void)esp_cache_msync(buffer + at, n, flags);
                }
                return;
        }

        // The 2D-DMA's chain for captures: made with the first watcher, stopped with the last.
        constexpr size_t                  ChainPieces = 64;
        constexpr unsigned int            DmaTimeoutMs = 500;
        constinit StaticObject<BlitChain> chain;

        struct Job {
                        Watcher              *watcher = nullptr;
                        ScreenWatch::Capture *capture = nullptr;
        };

        // The rectangles copied by the 2D-DMA from @p from (its rows repeated to @p lines as the scan
        // repeats them) into the buffer, packed. False: not done (the CPU copies instead).
        bool copyByDma(ScreenWatch::Capture &c, const Surface &from, const uint8_t *pixels, uint16_t lines) {
                if (!chain.isCreated() || !chain->isStarted()) return false;
                const BlitChain::Picture src{pixels, from.width(), from.height(), 3};
                const uint16_t           rows = from.height();
                size_t                   total = 0;
                for (size_t i = 0; i < c.count; ++i) total += size_t(c.rects[i].width) * c.rects[i].height * 3;
                const size_t synced = (total + CacheLine - 1) & ~(CacheLine - 1); // within capacity: aligned
                syncForDma(c.buffer, synced, false);
                chain->clear();
                Error  err;
                size_t at = 0;
                auto   add = [&](const BlitChain::Picture &to, uint16_t x, uint16_t sy, uint16_t dy, uint16_t w,
                               uint16_t h) {
                        if (err.isError()) return;
                        err = chain->add(src, x, sy, to, 0, dy, w, h);
                        if (err == Error::Full) {
                                err = chain->run(DmaTimeoutMs);
                                if (err.isOk()) err = chain->add(src, x, sy, to, 0, dy, w, h);
                        }
                        return;
                };
                for (size_t i = 0; i < c.count && err.isOk(); ++i) {
                        const Rect              &r = c.rects[i];
                        const BlitChain::Picture to{c.buffer + at, r.width, r.height, 3};
                        if (lines == rows) {
                                add(to, r.x, r.y, 0, r.width, r.height);
                        } else {
                                // Runs of screen rows on consecutive source rows go as one piece.
                                uint16_t start = 0;
                                uint32_t first = uint32_t(r.y) * rows / lines;
                                for (uint16_t k = 1; k <= r.height; ++k) {
                                        const uint32_t sr = k < r.height ? uint32_t(r.y + k) * rows / lines : 0;
                                        if (k < r.height && sr == first + (k - start)) continue;
                                        add(to, r.x, uint16_t(first), start, r.width, uint16_t(k - start));
                                        start = k;
                                        first = sr;
                                }
                        }
                        at += size_t(r.width) * r.height * 3;
                }
                if (err.isOk() && chain->count() > 0) err = chain->run(DmaTimeoutMs);
                syncForDma(c.buffer, synced, true);
                if (err.isError()) return false;
                c.bytes = at;
                return true;
        }

        // Rectangles taken from watcher @p w's tiles (all with `Capture::all`), in @p shown's
        // screen rows. Lock held.
        void take(Watcher &w, ScreenWatch::Capture &c, const Surface &shown, size_t bpp) {
                follow(w, shown);
                if (c.all) w.tiles.markAll();
                c.count = w.tiles.takeRects(c.rects, uint32_t(c.capacity / bpp));
                c.more = !w.tiles.isEmpty();
                if (!c.more) w.notified = false; // the next change tells it again
                return;
        }

        // The rectangles copied from @p from (@p shown's buffer @p from, rows mapped to @p lines):
        // by the 2D-DMA when the buffer allows it (B, G, R, on cache lines); else by the CPU.
        void copy(ScreenWatch::Capture &c, const Surface &shown, const uint8_t *from, uint16_t lines) {
                const size_t   bpp = c.format == ScreenWatch::Format::Rgb565 ? 2 : 3;
                const uint16_t rows = shown.height();
                c.width = shown.width();
                c.height = lines;
                const bool aligned =
                        (uintptr_t(c.buffer) & (CacheLine - 1)) == 0 && (c.capacity & (CacheLine - 1)) == 0;
                if (bpp == 3 && aligned && copyByDma(c, shown, from, lines)) return;
                const size_t stride = shown.stride();
                uint8_t     *to = c.buffer;
                for (size_t i = 0; i < c.count; ++i) {
                        const Rect    &r = c.rects[i];
                        const uint32_t s0 = uint32_t(r.y) * rows / lines;
                        const uint32_t s1 = uint32_t(r.bottom() - 1) * rows / lines + 1;
                        syncRows(from, stride, s0, s1);
                        for (uint16_t y = 0; y < r.height; ++y) {
                                const uint32_t src = uint32_t(r.y + y) * rows / lines;
                                const uint8_t *row = from + size_t(src) * stride + size_t(r.x) * 3;
                                if (bpp == 3) {
                                        std::memcpy(to, row, size_t(r.width) * 3);
                                        to += size_t(r.width) * 3;
                                        continue;
                                }
                                for (uint16_t x = 0; x < r.width; ++x, row += 3, to += 2) {
                                        const uint16_t p =
                                                uint16_t((row[2] >> 3) << 11 | (row[1] >> 2) << 5 | row[0] >> 3);
                                        to[0] = uint8_t(p);
                                        to[1] = uint8_t(p >> 8);
                                }
                        }
                }
                c.bytes = size_t(to - c.buffer);
                return;
        }

        // Under the window manager's capture (between its rounds, its last present shown): a
        // program's full screen, or the desktop before a present was seen. The full screen goes on
        // presenting meanwhile: its swaps held (one begun ends at the next frame's start).
        void copyOut(void *arg, const Surface &desktop) {
                Job                  &job = *static_cast<Job *>(arg);
                ScreenWatch::Capture &c = *job.capture;
                const Surface        *shown = scanned();
                if (shown == nullptr) shown = &desktop;
                auto *held = shown != &desktop ? const_cast<Surface *>(shown) : nullptr;
                if (held != nullptr) {
                        held->holdSwaps(true);
                        (void)held->waitFrame(held->frames(), HoldSettleMs);
                }
                const size_t   bpp = c.format == ScreenWatch::Format::Rgb565 ? 2 : 3;
                const uint16_t lines = shownRows(*shown);
                {
                        Mutex::Locker locker(lock(), WaitForever);
                        take(*job.watcher, c, *shown, bpp);
                }
                copy(c, *shown, shown->shown(), lines);
                if (held != nullptr) held->holdSwaps(false);
                return;
        }

        // The desktop's latest present copied with its swaps held, no window manager's lock and no
        // wait (devplan/vnc.md §16): the buffer holding that present stays as it is while the swaps
        // are held, shown or waiting (its drawer waits for the swap before drawing again), and the
        // tiles taken are its and earlier ones'; a present after the take marks tiles for the next.
        // The window manager draws its next round meanwhile. False: no present seen yet.
        bool copyLatest(Job &job, Surface &desktop) {
                ScreenWatch::Capture &c = *job.capture;
                const size_t          bpp = c.format == ScreenWatch::Format::Rgb565 ? 2 : 3;
                desktop.holdSwaps(true);
                const uint8_t *from = nullptr;
                {
                        Mutex::Locker locker(lock(), WaitForever);
                        if (latestSurface == &desktop) {
                                from = latestBuffer;
                                take(*job.watcher, c, desktop, bpp);
                        }
                }
                if (from != nullptr) copy(c, desktop, from, desktop.height());
                desktop.holdSwaps(false);
                return from != nullptr;
        }

} // namespace

Result<uint8_t> ScreenWatch::add(Notify notify, void *arg, StringView owner, uint32_t client) {
        if (notify == nullptr) return makeError<uint8_t>(Error::Invalid);
        auto [d, derr] = Display::display(0);
        if (derr.isError() || !d->isRunning()) return makeError<uint8_t>(Error::NotReady);
        Mutex::Locker locker(lock(), WaitForever);
        if (watchers == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, MaxWatchers * sizeof(Watcher));
                if (mem == nullptr) return makeError<uint8_t>(Error::NoMem);
                watchers = ::new (mem) Watcher[MaxWatchers];
        }
        display = d;
        for (size_t i = 0; i < MaxWatchers; ++i) {
                Watcher &w = watchers[i];
                if (w.used) continue;
                w = Watcher();
                w.used = true;
                w.notify = notify;
                w.arg = arg;
                w.client = client;
                const size_t n = owner.size() < OwnerBytes - 1 ? owner.size() : OwnerBytes - 1;
                std::memcpy(w.owner, owner.data(), n);
                w.owner[n] = '\0';
                const Surface *s = scanned();
                if (s != nullptr) follow(w, *s); // all marked
                w.notified = true;
                notify(arg);
                if (active.fetch_add(1, std::memory_order_acq_rel) == 0) {
                        latestSurface = nullptr; // presents weren't followed meanwhile
                        Surface::setPresentHook(presented, nullptr);
                        if (!chain.isCreated()) chain.create();
                        (void)chain->start(BlitChain::Conversion::None, ChainPieces); // failed: the CPU copies
                }
                countChanged();
                return makeResult(uint8_t(i));
        }
        return makeError<uint8_t>(Error::Full);
}

void ScreenWatch::remove(uint8_t id) {
        if (id >= MaxWatchers || watchers == nullptr) return;
        Mutex::Locker locker(lock(), WaitForever);
        Watcher      &w = watchers[id];
        if (!w.used) return;
        w.used = false;
        w.notify = nullptr;
        if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                Surface::setPresentHook(nullptr, nullptr);
                if (chain.isCreated()) chain->stop(); // its 2D-DMA pool given back
        }
        countChanged();
        return;
}

size_t ScreenWatch::count() {
        return active.load(std::memory_order_acquire);
}

bool ScreenWatch::owner(size_t index, char *out, size_t outBytes) {
        if (out == nullptr || outBytes == 0 || watchers == nullptr) return false;
        Mutex::Locker locker(lock(), WaitForever);
        size_t        seen = 0;
        for (size_t i = 0; i < MaxWatchers; ++i) {
                if (!watchers[i].used) continue;
                if (seen++ != index) continue;
                std::strncpy(out, watchers[i].owner, outBytes - 1);
                out[outBytes - 1] = '\0';
                return true;
        }
        return false;
}

bool ScreenWatch::isWatching(uint32_t client) {
        if (watchers == nullptr || active.load(std::memory_order_acquire) == 0) return false;
        Mutex::Locker locker(lock(), WaitForever);
        for (size_t i = 0; i < MaxWatchers; ++i)
                if (watchers[i].used && watchers[i].client == client) return true;
        return false;
}

size_t ScreenWatch::clients(uint32_t *out, size_t max) {
        if (watchers == nullptr) return 0;
        Mutex::Locker locker(lock(), WaitForever);
        size_t        n = 0;
        for (size_t i = 0; i < MaxWatchers; ++i) {
                if (!watchers[i].used) continue;
                if (n < max) out[n] = watchers[i].client;
                ++n;
        }
        return n;
}

void ScreenWatch::setActivity(Activity activity, void *arg) {
        Mutex::Locker locker(lock(), WaitForever);
        activityFn = activity;
        activityArg = arg;
        return;
}

Error ScreenWatch::capture(uint8_t id, Capture &c, unsigned int timeoutMs) {
        c.count = 0;
        c.bytes = 0;
        c.more = false;
        if (id >= MaxWatchers || watchers == nullptr) return Error::NotExist;
        {
                Mutex::Locker locker(lock(), WaitForever);
                if (!watchers[id].used) return Error::NotExist;
        }
        if (display == nullptr || !display->isRunning()) return Error::NotReady;
        Job job;
        job.watcher = &watchers[id];
        job.capture = &c;
        Surface &desktop = display->surface();
        if (scanned() == &desktop && desktop.buffering() == Surface::Buffering::Double && copyLatest(job, desktop))
                return Error::Ok;
        auto [wm, werr] = WindowManager::manager(0);
        if (werr.isOk() && wm->isRunning()) return wm->capture(copyOut, &job, timeoutMs);
        copyOut(&job, display->surface());
        return Error::Ok;
}

BMDS_NAMESPACE_END
