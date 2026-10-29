/**
 * @file      memory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <bmds/board.h>
#include <bmds/detail/platform.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/pagewindow.h>
#include <bmds/psramwindow.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include "memorywindow.h"

// IDF's private list of heaps (TLSF's bitmaps give the largest free block), its lock type first.
#include <multi_heap_platform.h>

#include <heap_private.h>

BMDS_DEBUG(Memory)

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t capsFor(MemSpace space) {
                return board().memSpace(space).heapCaps;
        }

        constinit std::atomic<Memory::Reclaimer *> reclaimers[Memory::MaxReclaimers] = {};

        // Each cache's counters (devplan/sysmon.md §6), cache.ITEMS: names are an interface once shipped.
        constexpr Stat::Desc CacheStats[] = {
                {"bytes", Stat::Unit::Bytes, Stat::Kind::Level, Stat::KeepHistory, {}, "", "Bytes it holds"},
                {"items", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Items it holds"},
                {"reclaimable", Stat::Unit::Bytes, Stat::Kind::Level, 0, {}, "", "Bytes it could give back now"},
                {"hits", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Lookups that found an item"},
                {"misses", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Lookups that didn't"},
                {"reclaims",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {},
                 "",
                 "Times it gave memory back to an allocation that failed"},
        };

        void readCache(void *arg, Span<int64_t> v) {
                const Memory::Reclaimer *r = reclaimers[reinterpret_cast<uintptr_t>(arg)].load();
                if (r == nullptr) return;
                const Memory::Reclaimer::Usage u = r->usage();
                v[0] = int64_t(u.bytes);
                v[1] = int64_t(u.count);
                v[2] = int64_t(u.reclaimable);
                v[3] = u.hits;
                v[4] = u.misses;
                v[5] = u.reclaims;
                return;
        }

        struct CacheCounters {
                        Stats::Group groups[Memory::MaxReclaimers] = {
                                {CacheStats, 6, readCache, reinterpret_cast<void *>(uintptr_t(0))},
                                {CacheStats, 6, readCache, reinterpret_cast<void *>(uintptr_t(1))},
                                {CacheStats, 6, readCache, reinterpret_cast<void *>(uintptr_t(2))},
                                {CacheStats, 6, readCache, reinterpret_cast<void *>(uintptr_t(3))},
                        };
        };
        static_assert(Memory::MaxReclaimers == 4 && sizeof(CacheStats) / sizeof(CacheStats[0]) == 6);

        constinit StaticObject<CacheCounters> cacheCounters;
        constinit StaticObject<Mutex>         cacheCountersLock; // made by the first reclaimer (at boot)

        // A failed allocation in @p space: true if the reclaimers freed something to try again with.
        // Each in turn, until what was asked is freed.
        bool reclaimed(MemSpace space, size_t sizeBytes) {
                if (space != MemSpace::Psram && space != MemSpace::PsramDma) return false;
                size_t freed = 0;
                for (auto &slot : reclaimers) {
                        Memory::Reclaimer *r = slot.load();
                        if (r != nullptr)
                                freed += r->reclaim(space, sizeBytes - (freed < sizeBytes ? freed : sizeBytes));
                        if (freed >= sizeBytes) break;
                }
                return freed != 0;
        }

        constexpr size_t CacheLine = Memory::DmaAlign;

        // Large blocks, logged with `log/debug` Memory: what a PSRAM window would take
        // (devplan/psramwindow.md step 1), the caller's address for bmds-panic's addr2line.
        constexpr size_t LargeBytes = 64 * 1024;

        // A DMA buffer's size: whole cache lines, and one more. The heap keeps
        // the next block's link in the last word of a block (TLSF's
        // prev_phys_block) and writes it through the cache now and then: in a
        // buffer the DMA writes, that clobbered its last bytes and wrote stale
        // copies of its last line back (devplan/display.md §5c). With a line of
        // its own after the buffer, no heap write shares a line with it.
        size_t dmaSize(MemSpace space, size_t sizeBytes) {
                if (space != MemSpace::InternalDma && space != MemSpace::PsramDma) return sizeBytes;
                return (sizeBytes + CacheLine - 1) / CacheLine * CacheLine + CacheLine;
        }

        // ---- The heap's largest free block, from TLSF's bitmaps (devplan/psramwindow.md §2a)

        // multi_heap.c's and TLSF's private structures as this IDF has them. Checked once against
        // IDF's own walk (heap_caps_get_largest_free_block()); if they disagree, the walk's figure
        // as the window's policy last saw it is used instead.
        struct MultiHeap {
                        void  *lock;
                        size_t freeBytes;
                        size_t minimumFreeBytes;
                        size_t poolSize;
                        void  *heapData;
        };
        struct TlsfControl {
                        void         *blockNull[4];
                        unsigned int  flIndexCount : 5;
                        unsigned int  flIndexShift : 3;
                        unsigned int  flIndexMax : 6;
                        unsigned int  slIndexCount : 6;
                        unsigned int  slIndexCountLog2 : 3;
                        unsigned int  smallBlockSize : 8;
                        size_t        size;
                        unsigned int  flBitmap;
                        unsigned int *slBitmap;
        };

        constexpr size_t MaxPsramHeaps = 4;

        struct Tlsf {
                        const TlsfControl *controls[MaxPsramHeaps] = {};
                        size_t             count = 0;
                        bool               usable = false;
        };

        constinit Tlsf tlsf;

        int fls(unsigned int word) {
                return word != 0 ? 31 - __builtin_clz(word) : -1;
        }

        // A free list's smallest size: the largest free block is at least this.
        size_t bitmapLargest(const TlsfControl &c) {
                const int fl = fls(c.flBitmap);
                if (fl <= 0) return 0;
                const int    sl = fls(c.slBitmap[fl]);
                const int    raw = fl + int(c.flIndexShift) - 1;
                const size_t base = size_t(1) << raw;
                return base + (size_t(sl < 0 ? 0 : sl) << (raw - int(c.slIndexCountLog2)));
        }

        // Found and checked once the window starts (boot, one task).
        void findTlsf() {
                heap_t *h = nullptr;
                SLIST_FOREACH(h, &registered_heaps, next) {
                        if (h->start < intptr_t(SOC_EXTRAM_LOW) || h->start >= intptr_t(SOC_EXTRAM_HIGH)) continue;
                        if (tlsf.count == MaxPsramHeaps) return; // more than expected: the walk instead
                        tlsf.controls[tlsf.count++] = static_cast<const TlsfControl *>(
                                reinterpret_cast<const MultiHeap *>(h->heap)->heapData);
                }
                size_t mine = 0;
                for (size_t i = 0; i < tlsf.count; ++i) {
                        const size_t b = bitmapLargest(*tlsf.controls[i]);
                        if (b > mine) mine = b;
                }
                const size_t walked = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                // A lower bound within a 32nd of the size (19 first-level lists of 32 here).
                tlsf.usable = tlsf.count != 0 && mine <= walked && mine >= walked - walked / 16;
                if (!tlsf.usable)
                        bmdsWarn("Memory: TLSF's bitmaps say %u, the walk %u: the walk for the largest block",
                                 unsigned(mine), unsigned(walked));
                return;
        }

        // A lower bound on the PSRAM heap's largest free block: O(1), without the heap's lock (a
        // moment's inconsistency only sways the policy).
        size_t heapLargest() {
                if (!tlsf.usable) return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                size_t largest = 0;
                for (size_t i = 0; i < tlsf.count; ++i) {
                        const size_t b = bitmapLargest(*tlsf.controls[i]);
                        if (b > largest) largest = b;
                }
                return largest;
        }

        // ---- Large blocks through the PSRAM window (devplan/psramwindow.md §3b, §3c)

        constexpr size_t Page = PsramWindow::PageBytes;

        // Above this the window always: mapping costs about 6 µs a megabyte (§2a).
        constexpr size_t AlwaysBytes = 256 * 1024;

        // DMA blocks through the window: the engines their buffers go to read and write window
        // addresses (§2b: the DW-GDMA scan, the 2D-DMA, the PPA, JPEG, the AXI GDMA, the card).
        // Were one not to, they would stay on the heap, which would keep contiguous room for a
        // full-screen switch's two surfaces (§3f).
        constexpr bool   DmaThroughWindow = true;
        constexpr size_t ContiguousReserve = DmaThroughWindow ? 0 : size_t(6) << 20;

        // A block's record: its range and the heap runs behind it, in order.
        struct WindowBlock {
                        uint8_t      *at = nullptr;
                        uint32_t      pages = 0;
                        size_t        bytes = 0;
                        uint32_t      runCount = 0;
                        PageRuns::Run runs[PageRuns::MaxRuns];
        };

        struct Window {
                        Mutex                 lock;                              // the records, gathering and mapping
                        WindowBlock          *blocks[PageWindow::MaxPages] = {}; // by the range's first page
                        Memory::WindowUsage   counts;
                        Memory::WindowHistory history; // its seconds unused: from historyStartUs
                        int64_t               historyStartUs = 0;
                        uint32_t              historySequence = 0;
        };

        constinit StaticObject<Window> window; // made when the window starts (boot)

        // The history across resets (§3g): two copies in RTC memory, which a reset leaves alone and
        // the caches don't hold (the CPU caches only addresses below the peripherals', so nothing
        // is lost dirty). Each is written whole, its magic last, so a reset partway leaves the
        // other. Power-up leaves garbage, and another firmware its own things: the magic, size and
        // check tell. Bytes, not the struct: no constructor may run over it.
        struct HistoryCopy {
                        uint32_t magic;
                        uint32_t size;
                        uint32_t sequence;
                        uint32_t reserved;
                        int64_t  startUs; // Detail::persistentTimeUs() when it started
                        alignas(8) uint8_t counts[sizeof(Memory::WindowHistory)];
                        uint32_t check;
        };

        constexpr uint32_t HistoryMagic = 0x48574d42; // "BMWH"

        RTC_NOINIT_ATTR HistoryCopy historyCopies[2];

        uint32_t checkOf(const HistoryCopy &c) {
                // FNV-1a over everything before the check.
                const auto *bytes = reinterpret_cast<const uint8_t *>(&c);
                uint32_t    h = 2166136261u;
                for (size_t i = 0; i < offsetof(HistoryCopy, check); ++i) {
                        h ^= bytes[i];
                        h *= 16777619u;
                }
                return h;
        }

        // Writes the history into the older copy. Under the window's lock.
        void saveHistory(Window &w) {
                HistoryCopy &c = historyCopies[++w.historySequence & 1];
                c.magic = 0;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.size = sizeof(HistoryCopy);
                c.sequence = w.historySequence;
                c.reserved = 0;
                c.startUs = w.historyStartUs;
                std::memcpy(c.counts, &w.history, sizeof(c.counts));
                c.check = checkOf(c);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.magic = HistoryMagic;
                return;
        }

        void startHistory(Window &w) {
                w.history = Memory::WindowHistory{};
                w.history.boots = 1;
                w.historyStartUs = Detail::persistentTimeUs();
                return;
        }

        // At boot: the newer good copy, this boot counted, or a new history.
        void loadHistory(Window &w) {
                const HistoryCopy *newest = nullptr;
                for (const HistoryCopy &c : historyCopies) {
                        if (c.magic != HistoryMagic || c.size != sizeof(HistoryCopy) || c.check != checkOf(c)) continue;
                        if (newest == nullptr || int32_t(c.sequence - newest->sequence) > 0) newest = &c;
                }
                if (newest != nullptr && newest->startUs <= Detail::persistentTimeUs()) {
                        std::memcpy(&w.history, newest->counts, sizeof(w.history));
                        w.history.boots += 1;
                        w.historyStartUs = newest->startUs;
                        w.historySequence = newest->sequence;
                } else {
                        startHistory(w);
                }
                saveHistory(w);
                return;
        }

        template <typename T> void raise(T &peak, T value) {
                if (value > peak) peak = value;
                return;
        }

        // A block mapped or grown: the history's peaks. Under the window's lock.
        void notePeaks(Window &w, uint32_t runs) {
                Memory::WindowHistory &h = w.history;
                raise(h.peakUsed, PsramWindow::usage().used);
                raise(h.peakBlocks, w.counts.blocks);
                raise(h.mostRuns, runs);
                return;
        }

        // Runs from the PSRAM heap: whole pages, aligned, and a line after them that the heap's
        // own write into a block's last word lands in (never a mapped page).
        class HeapRuns final : public PageRuns::Source {
                public:
                        size_t   largestFree() override { return heapLargest(); }
                        uint8_t *allocRun(uint32_t pages) override {
                                return static_cast<uint8_t *>(heap_caps_aligned_alloc(
                                        Page, size_t(pages) * Page + CacheLine, capsFor(MemSpace::Psram)));
                        }
                        void freeRun(uint8_t *at) override {
                                heap_caps_free(at);
                                return;
                        }
        };

        size_t roundToPages(size_t bytes) {
                return (bytes + Page - 1) / Page * Page;
        }

        // Maps runs @p from on at @p at, each run's old lines dropped first, then the range's.
        void mapRuns(uint8_t *at, const PageRuns::Run *runs, size_t count) {
                size_t off = 0;
                for (size_t i = 0; i < count; ++i) {
                        PsramWindow::invalidate(runs[i].at, size_t(runs[i].pages) * Page);
                        PsramWindow::mapHeap(at + off, runs[i].at, runs[i].pages);
                        off += size_t(runs[i].pages) * Page;
                }
                PsramWindow::invalidate(at, off);
                return;
        }

        // Unmaps @p pages pages at @p at: their lines dropped (the contents go), entries cleared.
        void unmapPages(uint8_t *at, uint32_t pages) {
                PsramWindow::invalidate(at, size_t(pages) * Page);
                PsramWindow::clear(at, pages);
                return;
        }

        void *windowAlloc(size_t sizeBytes, bool fromTop) {
                if (!window.isCreated()) return nullptr;
                Window        &w = *window;
                const uint32_t pages = uint32_t(roundToPages(sizeBytes) / Page);
                Mutex::Locker  locker(w.lock, WaitForever);
                uint8_t       *at = PsramWindow::takeRange(pages, fromTop);
                if (at == nullptr) return nullptr;
                auto *b = static_cast<WindowBlock *>(heap_caps_malloc(sizeof(WindowBlock), capsFor(MemSpace::Psram)));
                HeapRuns     heap;
                const size_t n =
                        b != nullptr ? PageRuns::gather(heap, pages, uint32_t(Page), Span<PageRuns::Run>(b->runs)) : 0;
                if (n == 0) {
                        heap_caps_free(b);
                        PsramWindow::giveRange(at, pages);
                        return nullptr;
                }
                b->at = at;
                b->pages = pages;
                b->bytes = sizeBytes;
                b->runCount = uint32_t(n);
                mapRuns(at, b->runs, n);
                w.blocks[(at - PsramWindow::base()) / Page] = b;
                w.counts.blocks += 1;
                w.counts.runs += uint32_t(n);
                w.counts.bytes += sizeBytes;
                notePeaks(w, uint32_t(n));
                saveHistory(w);
                return at;
        }

        WindowBlock *&recordOf(Window &w, const void *p) {
                return w.blocks[(static_cast<const uint8_t *>(p) - PsramWindow::base()) / Page];
        }

        // A block from windowAlloc(): unmapped, its runs back to the heap. False if @p ptr isn't one.
        bool windowFree(void *ptr) {
                if (!window.isCreated() || !PsramWindow::contains(ptr, 0)) return false;
                Window       &w = *window;
                Mutex::Locker locker(w.lock, WaitForever);
                WindowBlock *&slot = recordOf(w, ptr);
                WindowBlock  *b = slot;
                if (b == nullptr || b->at != ptr) return true; // not a block's start: a bad free, ignored
                unmapPages(b->at, b->pages);
                HeapRuns heap;
                PageRuns::release(heap, Span<const PageRuns::Run>(b->runs, b->runCount));
                PsramWindow::giveRange(b->at, b->pages);
                w.counts.blocks -= 1;
                w.counts.runs -= b->runCount;
                w.counts.bytes -= b->bytes;
                slot = nullptr;
                heap_caps_free(b);
                return true;
        }

        // Grows or shrinks a window block in place. False if it can't (the caller copies).
        bool windowResize(void *ptr, size_t sizeBytes) {
                Window       &w = *window;
                Mutex::Locker locker(w.lock, WaitForever);
                WindowBlock  *b = recordOf(w, ptr);
                if (b == nullptr || b->at != ptr) return false;
                const uint32_t pages = uint32_t(roundToPages(sizeBytes) / Page);
                if (pages <= b->pages) {
                        // Whole runs past the new end go back.
                        uint32_t end = b->pages;
                        while (b->runCount > 1 && end - b->runs[b->runCount - 1].pages >= pages) {
                                const PageRuns::Run &last = b->runs[b->runCount - 1];
                                end -= last.pages;
                                unmapPages(b->at + size_t(end) * Page, last.pages);
                                HeapRuns heap;
                                heap.freeRun(last.at);
                                PsramWindow::giveRange(b->at + size_t(end) * Page, last.pages);
                                b->runCount -= 1;
                                w.counts.runs -= 1;
                        }
                        b->pages = end;
                        w.counts.bytes = w.counts.bytes - b->bytes + sizeBytes;
                        b->bytes = sizeBytes;
                        return true;
                }
                const uint32_t extra = pages - b->pages;
                uint8_t       *tail = b->at + size_t(b->pages) * Page;
                if (b->runCount == PageRuns::MaxRuns || !PsramWindow::takeRangeAt(tail, extra)) return false;
                HeapRuns     heap;
                const size_t n =
                        PageRuns::gather(heap, extra, uint32_t(Page),
                                         Span<PageRuns::Run>(b->runs + b->runCount, PageRuns::MaxRuns - b->runCount));
                if (n == 0) {
                        PsramWindow::giveRange(tail, extra);
                        return false;
                }
                mapRuns(tail, b->runs + b->runCount, n);
                b->runCount += uint32_t(n);
                b->pages = pages;
                w.counts.runs += uint32_t(n);
                w.counts.bytes = w.counts.bytes - b->bytes + sizeBytes;
                b->bytes = sizeBytes;
                w.counts.grownInPlace += 1;
                w.history.grownInPlace += 1;
                notePeaks(w, b->runCount);
                saveHistory(w);
                return true;
        }

        // Whether a request may go through the window at all (§3c rules 1-3, and what the window
        // can't take: DMA blocks until verified, alignments past a page, blocks kept off it).
        bool windowable(MemSpace space, size_t r, const Memory::Options &o, bool strict) {
                if (!window.isCreated() || o.heapOnly || o.alignBytes > Page || r < Page) return false;
                if (space != MemSpace::Psram && !(DmaThroughWindow && space == MemSpace::PsramDma)) return false;
                const size_t rounded = roundToPages(r);
                if (strict && rounded - r > r / 8) return false; // rule 2: little lost to rounding
                const PsramWindow::Usage u = PsramWindow::usage();
                if (u.longest < rounded / Page) return false;          // rule 3: room
                return !strict || u.longest >= (u.pages - u.used) / 4; // its own addresses not cut up
        }

        enum class Route : uint8_t {
                Heap,
                Size,
                Fit,
                CutUp,
                Reserve
        };

        // §3c rules 4-7: the window for a large block, or when the heap can't comfortably give it.
        // The heap's largest free block and free bytes in @p largest and @p free (the history's).
        Route choose(size_t r, size_t &largest, size_t &free) {
                largest = heapLargest();
                free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                if (r >= AlwaysBytes) return Route::Size;
                if (r > largest / 2) return Route::Fit;
                if (largest < free / 2) return Route::CutUp;
                if (largest - r < ContiguousReserve) return Route::Reserve;
                return Route::Heap;
        }

        void count(Route route, size_t largest, size_t free) {
                if (!window.isCreated()) return;
                Window       &w = *window;
                Mutex::Locker locker(w.lock, WaitForever);
                switch (route) {
                        case Route::Heap:
                                ++w.counts.byHeap;
                                ++w.history.byHeap;
                                break;
                        case Route::Size:
                                ++w.counts.bySize;
                                ++w.history.bySize;
                                break;
                        case Route::Fit:
                                ++w.counts.byFit;
                                ++w.history.byFit;
                                break;
                        case Route::CutUp:
                                ++w.counts.byCutUp;
                                ++w.history.byCutUp;
                                break;
                        case Route::Reserve:
                                ++w.counts.byReserve;
                                ++w.history.byReserve;
                                break;
                }
                Memory::WindowHistory &h = w.history;
                if (h.freeThen == 0 || largest < h.lowestLargest) {
                        h.lowestLargest = largest;
                        h.freeThen = free;
                }
                saveHistory(w);
                return;
        }

        void countFallback(bool toWindow) {
                Window       &w = *window;
                Mutex::Locker locker(w.lock, WaitForever);
                ++(toWindow ? w.counts.toWindow : w.counts.toHeap);
                ++(toWindow ? w.history.toWindow : w.history.toHeap);
                saveHistory(w);
                return;
        }

        // A large request nothing could give.
        void countFailure() {
                if (!window.isCreated()) return;
                Window       &w = *window;
                Mutex::Locker locker(w.lock, WaitForever);
                ++w.history.failed;
                saveHistory(w);
                return;
        }

        void *heapAlloc(MemSpace space, size_t sizeBytes, size_t alignBytes) {
                if (alignBytes == 0) return heap_caps_malloc(dmaSize(space, sizeBytes), capsFor(space));
                return heap_caps_aligned_alloc(alignBytes, dmaSize(space, sizeBytes), capsFor(space));
        }

        // One try by the policy's route, then the other route.
        void *place(MemSpace space, size_t sizeBytes, const Memory::Options &o) {
                const bool fits = windowable(space, sizeBytes, o, true);
                const bool can = fits || windowable(space, sizeBytes, o, false);
                if (fits) {
                        size_t      largest = 0;
                        size_t      free = 0;
                        const Route route = choose(sizeBytes, largest, free);
                        count(route, largest, free);
                        if (route != Route::Heap) {
                                void *p = windowAlloc(sizeBytes, o.lifetime == Memory::Lifetime::System);
                                if (p != nullptr) return p;
                                p = heapAlloc(space, sizeBytes, o.alignBytes);
                                if (p != nullptr) countFallback(false);
                                return p;
                        }
                }
                void *p = heapAlloc(space, sizeBytes, o.alignBytes);
                if (p == nullptr && can) {
                        p = windowAlloc(sizeBytes, o.lifetime == Memory::Lifetime::System);
                        if (p != nullptr) countFallback(true);
                }
                return p;
        }

} // namespace

Error MemoryWindow::init() {
        if (window.isCreated() || !PsramWindow::isReady()) return Error::Ok;
        findTlsf();
        loadHistory(window.create());
        return Error::Ok;
}

void *Memory::alloc(MemSpace space, size_t sizeBytes, const Options &options) {
        void *p = place(space, sizeBytes, options);
        if (p == nullptr && reclaimed(space, sizeBytes + options.alignBytes)) p = place(space, sizeBytes, options);
        if (p == nullptr && sizeBytes >= LargeBytes) countFailure();
        if (sizeBytes >= LargeBytes)
                bmdsDebug("+%u %s %p%s from %p", unsigned(sizeBytes), memSpaceName(space), p,
                          PsramWindow::contains(p, 0) ? " (window)" : "", __builtin_return_address(0));
        return p;
}

void *Memory::alloc(MemSpace space, size_t sizeBytes) {
        return alloc(space, sizeBytes, Options{});
}

void *Memory::allocAligned(MemSpace space, size_t sizeBytes, size_t alignBytes) {
        Options o;
        o.alignBytes = alignBytes;
        return alloc(space, sizeBytes, o);
}

void *Memory::realloc(MemSpace space, void *ptr, size_t sizeBytes, size_t alignBytes) {
        Options o;
        o.alignBytes = alignBytes;
        if (ptr == nullptr) return alloc(space, sizeBytes, o);
        if (sizeBytes == 0) {
                free(ptr);
                return nullptr;
        }
        size_t old = 0;
        if (window.isCreated() && PsramWindow::contains(ptr, 0)) {
                // In place while the block may stay in the window (its space and alignment allow).
                if (windowable(space, Page, o, false) && windowResize(ptr, sizeBytes)) return ptr;
                Window            &w = *window;
                Mutex::Locker      locker(w.lock, WaitForever);
                const WindowBlock *b = recordOf(w, ptr);
                old = b != nullptr ? b->bytes : 0;
        } else {
                old = heap_caps_get_allocated_size(ptr);
        }
        void *grown = alloc(space, sizeBytes, o);
        if (grown == nullptr) return nullptr;
        std::memcpy(grown, ptr, old < sizeBytes ? old : sizeBytes);
        free(ptr);
        return grown;
}

void Memory::free(void *ptr) {
        if (ptr == nullptr) return;
#if BMDS_ENABLE_DEBUG_LOG
        if (bmdsDebugEnabled && !PsramWindow::contains(ptr, 0)) [[unlikely]] {
                const size_t bytes = heap_caps_get_allocated_size(ptr);
                if (bytes >= LargeBytes) bmdsDebug("-%u %p", unsigned(bytes), ptr);
        }
#endif
        if (windowFree(ptr)) return;
        heap_caps_free(ptr);
        return;
}

Memory::WindowUsage Memory::windowUsage() {
        WindowUsage u;
        if (!window.isCreated()) return u;
        {
                Mutex::Locker locker(window->lock, WaitForever);
                u = window->counts;
        }
        const PsramWindow::Usage r = PsramWindow::usage();
        u.pages = r.pages;
        u.used = r.used;
        u.longest = r.longest;
        return u;
}

Memory::WindowHistory Memory::windowHistory() {
        WindowHistory h;
        if (!window.isCreated()) return h;
        Window       &w = *window;
        Mutex::Locker locker(w.lock, WaitForever);
        h = w.history;
        h.seconds = uint64_t(Detail::persistentTimeUs() - w.historyStartUs) / 1000000;
        return h;
}

void Memory::clearWindowHistory() {
        if (!window.isCreated()) return;
        Window       &w = *window;
        Mutex::Locker locker(w.lock, WaitForever);
        startHistory(w);
        saveHistory(w);
        return;
}

Error Memory::addReclaimer(Reclaimer *r) {
        if (r == nullptr) return Error::Invalid;
        for (auto &slot : reclaimers)
                if (slot.load() == r) return Error::Invalid;
        for (size_t i = 0; i < MaxReclaimers; ++i) {
                Reclaimer *none = nullptr;
                if (!reclaimers[i].compare_exchange_strong(none, r)) continue;
                // Its counters, cache.ITEMS (the first reclaimer comes at boot, one task at a time).
                if (!cacheCountersLock.isCreated()) cacheCountersLock.create();
                {
                        Mutex::Locker locker(*cacheCountersLock, WaitForever);
                        if (!cacheCounters.isCreated()) cacheCounters.create();
                        Stats::Group &g = cacheCounters->groups[i];
                        char          name[Stats::MaxGroupName + 1];
                        std::snprintf(name, sizeof(name), "cache.%s", r->usage().items);
                        g.setName(name);
                        (void)Stats::add(g);
                }
                return Error::Ok;
        }
        return Error::Full;
}

void Memory::removeReclaimer(Reclaimer *r) {
        for (size_t i = 0; i < MaxReclaimers; ++i) {
                Reclaimer *was = r;
                if (!reclaimers[i].compare_exchange_strong(was, nullptr)) continue;
                if (cacheCountersLock.isCreated()) {
                        Mutex::Locker locker(*cacheCountersLock, WaitForever);
                        if (cacheCounters.isCreated()) Stats::remove(cacheCounters->groups[i]);
                }
        }
        return;
}


size_t Memory::reclaimerUsage(Span<Reclaimer::Usage> out) {
        size_t n = 0;
        for (auto &slot : reclaimers) {
                Reclaimer *r = slot.load();
                if (r == nullptr) continue;
                if (n < out.size()) out[n] = r->usage();
                ++n;
        }
        return n;
}

bool Memory::isExternal(const void *p, size_t sizeBytes) {
        if (esp_ptr_external_ram(p) &&
            (sizeBytes == 0 || esp_ptr_external_ram(static_cast<const uint8_t *>(p) + sizeBytes - 1)))
                return true;
        return PsramWindow::contains(p, sizeBytes);
}

MemStats Memory::stats(MemSpace space) {
        // Region queries see raw heap capabilities, without the adjustments
        // esp_mm applies on allocation: MALLOC_CAP_CACHE_ALIGNED is only an
        // alignment hint, and no region is marked SPIRAM|DMA (PSRAM is
        // DMA-capable on the P4, so esp_mm drops DMA for SPIRAM requests).
        uint32_t caps = capsFor(space) & ~MALLOC_CAP_CACHE_ALIGNED;
        if (caps & MALLOC_CAP_SPIRAM) caps &= ~MALLOC_CAP_DMA;
        multi_heap_info_t info = {};
        heap_caps_get_info(&info, caps);
        MemStats s;
        s.totalBytes = heap_caps_get_total_size(caps);
        s.freeBytes = info.total_free_bytes;
        s.minFreeBytes = info.minimum_free_bytes;
        s.largestFreeBlock = info.largest_free_block;
        // Through the window: its longest range, filled from the heap's free pages wherever they
        // are (two pages short of them: what runs lose around them, roughly).
        if (window.isCreated() && (space == MemSpace::Psram || (DmaThroughWindow && space == MemSpace::PsramDma))) {
                const PsramWindow::Usage u = PsramWindow::usage();
                const size_t             pages = s.freeBytes / Page > 2 ? s.freeBytes / Page - 2 : 0;
                const size_t             mapped = (u.longest < pages ? u.longest : pages) * Page;
                if (mapped > s.largestFreeBlock) s.largestFreeBlock = mapped;
        }
        return s;
}

BMDS_NAMESPACE_END
