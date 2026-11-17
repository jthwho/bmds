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
#include <bmds/mutex.h>
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

        // Watcher @p w's tiles of @p s: a new source (or size) is all new. Lock held.
        void follow(Watcher &w, const Surface &s) {
                if (w.source == &s && w.tiles.width() == s.width() && w.tiles.height() == s.height()) return;
                w.source = &s;
                if (!w.tiles.resize(s.width(), s.height())) w.tiles = ChangeTiles(); // too big: nothing kept
                return;
        }

        // Every surface's present: the scanned one's changes to each watcher.
        void presented(void *, const Surface &surface, const uint32_t *tiles, uint16_t rowWords, uint16_t rows) {
                if (active.load(std::memory_order_acquire) == 0 || &surface != scanned()) return;
                Mutex::Locker locker(lock(), WaitForever);
                for (size_t i = 0; i < ScreenWatch::MaxWatchers; ++i) {
                        Watcher &w = watchers[i];
                        if (!w.used) continue;
                        const bool fresh = w.source != &surface;
                        follow(w, surface);
                        if (!fresh) {
                                if (tiles == nullptr)
                                        w.tiles.markAll();
                                else
                                        w.tiles.orIn(tiles, rowWords, rows);
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

        // A rectangle's rows of @p from (@p stride bytes a row) fetched from PSRAM: the CPU never
        // writes a shown buffer, so dropping its lines loses nothing.
        void syncRows(const uint8_t *from, size_t stride, const Rect &r) {
                const uintptr_t begin = (uintptr_t(from) + size_t(r.y) * stride) & ~uintptr_t(CacheLine - 1);
                const uintptr_t end =
                        (uintptr_t(from) + size_t(r.bottom()) * stride + CacheLine - 1) & ~uintptr_t(CacheLine - 1);
                for (uintptr_t at = begin; at < end; at += SyncChunk) {
                        const size_t n = end - at < SyncChunk ? end - at : SyncChunk;
                        (void)esp_cache_msync(reinterpret_cast<void *>(at), n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                }
                return;
        }

        struct Job {
                        Watcher              *watcher = nullptr;
                        ScreenWatch::Capture *capture = nullptr;
        };

        // Under the window manager's capture: the shown picture holds still while it is copied.
        void copyOut(void *arg, const Surface &desktop) {
                Job                  &job = *static_cast<Job *>(arg);
                ScreenWatch::Capture &c = *job.capture;
                const Surface        *shown = scanned();
                if (shown == nullptr) shown = &desktop;
                // A program's full screen goes on presenting meanwhile: its swaps held (one begun
                // ends at the next frame's start), as a screenshot does.
                auto *held = shown != &desktop ? const_cast<Surface *>(shown) : nullptr;
                if (held != nullptr) {
                        held->holdSwaps(true);
                        (void)held->waitFrame(held->frames(), HoldSettleMs);
                }
                const size_t bpp = c.format == ScreenWatch::Format::Rgb565 ? 2 : 3;
                {
                        Mutex::Locker locker(lock(), WaitForever);
                        Watcher      &w = *job.watcher;
                        follow(w, *shown);
                        if (c.all) w.tiles.markAll();
                        c.count = w.tiles.takeRects(c.rects, uint32_t(c.capacity / bpp));
                        c.more = !w.tiles.isEmpty();
                        if (!c.more) w.notified = false; // the next change tells it again
                }
                c.width = shown->width();
                c.height = shown->height();
                const uint8_t *from = shown->shown();
                const size_t   stride = shown->stride();
                uint8_t       *to = c.buffer;
                for (size_t i = 0; i < c.count; ++i) {
                        const Rect &r = c.rects[i];
                        syncRows(from, stride, r);
                        for (uint16_t y = 0; y < r.height; ++y) {
                                const uint8_t *row = from + size_t(r.y + y) * stride + size_t(r.x) * 3;
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
                if (held != nullptr) held->holdSwaps(false);
                return;
        }

} // namespace

Result<uint8_t> ScreenWatch::add(Notify notify, void *arg, StringView owner) {
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
                const size_t n = owner.size() < OwnerBytes - 1 ? owner.size() : OwnerBytes - 1;
                std::memcpy(w.owner, owner.data(), n);
                w.owner[n] = '\0';
                const Surface *s = scanned();
                if (s != nullptr) follow(w, *s); // all marked
                w.notified = true;
                notify(arg);
                if (active.fetch_add(1, std::memory_order_acq_rel) == 0) Surface::setPresentHook(presented, nullptr);
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
        if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) Surface::setPresentHook(nullptr, nullptr);
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
        auto [wm, werr] = WindowManager::manager(0);
        if (werr.isOk() && wm->isRunning()) return wm->capture(copyOut, &job, timeoutMs);
        copyOut(&job, display->surface());
        return Error::Ok;
}

BMDS_NAMESPACE_END
