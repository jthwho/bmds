/**
 * @file      benchwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PSRAM window's measurements (devplan/psramwindow.md): what mapping heap pages into the
 * window costs by size, and a large block's allocation and free through `Memory`; the CPU and
 * the AXI GDMA through window addresses against the heap's own for the same physical pages; the
 * heap's largest free block by IDF's walk against TLSF's bitmaps. A run takes a range of the
 * window for itself and gives it back.
 */

#include <cstdio>
#include <cstring>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <bmds/bench.h>
#include <bmds/memory.h>
#include <bmds/psramwindow.h>
#include <bmds/semaphore.h>
#include <bmds/stopwatch.h>

// IDF's private heap list (a measurement's prototype), its lock type first.
#include <multi_heap_platform.h>

#include <heap_private.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t Page = PsramWindow::PageBytes;
        constexpr size_t Line = 128; // the L2 cache's line

        // A range of the window, held for one run.
        struct Range {
                        uint8_t *at = nullptr;
                        uint32_t pages = 0;
                        explicit Range(uint32_t count) : pages(count) { at = PsramWindow::takeRange(count, true); }
                        ~Range() {
                                if (at != nullptr) PsramWindow::giveRange(at, pages);
                        }
        };

        // A heap block of whole pages (one run), and a line after it for the heap's own writes.
        struct Run {
                        uint8_t *p = nullptr;
                        size_t   bytes = 0;
                        explicit Run(size_t size) : bytes(size) {
                                Memory::Options o;
                                o.alignBytes = Page;
                                o.heapOnly = true;
                                p = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size + Line, o));
                        }
                        ~Run() { Memory::free(p); }
        };

        // Each cache's lines of @p bytes at @p p dropped: one call, or a page at a time.
        void invalidate(uint8_t *p, size_t bytes, bool byPage) {
                if (!byPage) {
                        PsramWindow::invalidate(p, bytes);
                        return;
                }
                for (size_t off = 0; off < bytes; off += Page) PsramWindow::invalidate(p + off, Page);
                return;
        }

        // The heap's lines dropped, the entries written, the window's stale lines dropped.
        void map(uint8_t *window, const Run &run) {
                PsramWindow::invalidate(run.p, run.bytes);
                PsramWindow::mapHeap(window, run.p, uint32_t(run.bytes / Page));
                PsramWindow::invalidate(window, run.bytes);
                return;
        }

        // Its dirty lines dropped, then the entries made invalid.
        void unmap(uint8_t *window, size_t bytes) {
                PsramWindow::invalidate(window, bytes);
                PsramWindow::clear(window, uint32_t(bytes / Page));
                return;
        }

        // ---- Mapping's cost by size

        Error runMapping(BenchContext &ctx) {
                for (const size_t kb : {64u, 1024u, 8192u, 16384u}) {
                        Run   run(kb * 1024);
                        Range window(uint32_t(kb * 1024 / Page));
                        if (run.p == nullptr || window.at == nullptr) return Error::NoMem;
                        char name[40];
                        auto part = [&](const char *what, auto &&timed) {
                                std::snprintf(name, sizeof(name), "%s_%u_kb_us", what, unsigned(kb));
                                return ctx.measure(name, "us", BenchBetter::Lower, [&](double &v) {
                                        v = timed();
                                        return Error(Error::Ok);
                                });
                        };
                        Error err = part("map", [&] {
                                Stopwatch w;
                                map(window.at, run);
                                const double us = double(w.elapsedUs());
                                unmap(window.at, run.bytes);
                                return us;
                        });
                        if (err.isOk())
                                err = part("unmap", [&] {
                                        map(window.at, run);
                                        Stopwatch w;
                                        unmap(window.at, run.bytes);
                                        return double(w.elapsedUs());
                                });
                        if (err.isOk())
                                err = part("entries", [&] {
                                        Stopwatch w;
                                        PsramWindow::mapHeap(window.at, run.p, uint32_t(run.bytes / Page));
                                        const double us = double(w.elapsedUs());
                                        PsramWindow::clear(window.at, uint32_t(run.bytes / Page));
                                        return us;
                                });
                        if (err.isOk())
                                err = part("inval_paged", [&] {
                                        Stopwatch w;
                                        invalidate(run.p, run.bytes, true);
                                        return double(w.elapsedUs());
                                });
                        if (err.isOk())
                                err = part("inval_whole", [&] {
                                        Stopwatch w;
                                        invalidate(run.p, run.bytes, false);
                                        return double(w.elapsedUs());
                                });
                        // Through Memory, as anything large is now: the policy, the runs gathered,
                        // the record, the map; then the free.
                        if (err.isOk())
                                err = part("alloc", [&] {
                                        Stopwatch    w;
                                        void        *p = Memory::alloc(MemSpace::Psram, kb * 1024);
                                        const double us = double(w.elapsedUs());
                                        const bool   mapped = PsramWindow::contains(p, 0);
                                        Memory::free(p);
                                        return p == nullptr ? -1.0 : mapped ? us : -2.0;
                                });
                        if (err.isOk())
                                err = part("free", [&] {
                                        void     *p = Memory::alloc(MemSpace::Psram, kb * 1024);
                                        Stopwatch w;
                                        Memory::free(p);
                                        return p == nullptr ? -1.0 : double(w.elapsedUs());
                                });
                        if (err.isError()) return err;
                }
                ctx.note("map: the heap's lines dropped, the entries written, the window's lines dropped; alloc and "
                         "free: Memory's (-1: none, -2: from the heap)");
                return Error::Ok;
        }

        // ---- The CPU and the GDMA through the window, against the heap's addresses

        constexpr size_t SpeedBytes = 8u << 20;
        constexpr size_t Chunk = 64 * 1024;

        double mbPerS(size_t bytes, int64_t us) {
                return us > 0 ? double(bytes) / double(us) * 1e6 / (1024 * 1024) : 0;
        }

        uint32_t patternWord(size_t index, uint32_t seed) {
                return uint32_t(index) * 2654435761u ^ seed;
        }

        // A pattern written through @p p a chunk at a time, each pushed to PSRAM and dropped.
        void writePattern(uint8_t *p, uint32_t seed) {
                auto *w = reinterpret_cast<uint32_t *>(p);
                for (size_t off = 0; off < SpeedBytes; off += Chunk) {
                        for (size_t i = off / 4; i < (off + Chunk) / 4; ++i) w[i] = patternWord(i, seed);
                        (void)esp_cache_msync(p + off, Chunk,
                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                }
                return;
        }

        // Read back through @p p from PSRAM (dropped first): the words wrong.
        size_t checkPattern(uint8_t *p, uint32_t seed, size_t bytes = SpeedBytes) {
                invalidate(p, bytes, false);
                const auto *w = reinterpret_cast<const uint32_t *>(p);
                size_t      wrong = 0;
                for (size_t i = 0; i < bytes / 4; ++i) wrong += w[i] != patternWord(i, seed) ? 1 : 0;
                return wrong;
        }

        bool dmaDone(async_memcpy_handle_t, async_memcpy_event_t *, void *arg) {
                static_cast<Semaphore *>(arg)->releaseFromIsr();
                return false;
        }

        Error runSpeed(BenchContext &ctx) {
                Run   run(SpeedBytes);
                Range range(uint32_t(SpeedBytes / Page));
                if (run.p == nullptr || range.at == nullptr) return Error::NoMem;
                uint8_t *const window = range.at;
                map(window, run);
                size_t wrong = 0;
                Error  err = Error::Ok;
                for (int through = 0; through < 2 && err.isOk(); ++through) {
                        uint8_t    *p = through == 0 ? run.p : window;
                        const char *at = through == 0 ? "heap" : "window";
                        char        name[32];
                        std::snprintf(name, sizeof(name), "%s_write_mb_s", at);
                        err = ctx.measure(name, "MB/s", BenchBetter::Higher, [&](double &v) {
                                Stopwatch w;
                                writePattern(p, 0x5a5a0000u + unsigned(through));
                                v = mbPerS(SpeedBytes, w.elapsedUs());
                                return Error(Error::Ok);
                        });
                        if (err.isError()) break;
                        std::snprintf(name, sizeof(name), "%s_read_mb_s", at);
                        err = ctx.measure(name, "MB/s", BenchBetter::Higher, [&](double &v) {
                                invalidate(p, SpeedBytes, false);
                                Stopwatch   w;
                                const auto *words = reinterpret_cast<const volatile uint32_t *>(p);
                                uint32_t    sum = 0;
                                for (size_t i = 0; i < SpeedBytes / 4; i += 8)
                                        sum += words[i]; // a word a 32-byte stretch
                                v = mbPerS(SpeedBytes, w.elapsedUs());
                                (void)sum;
                                return Error(Error::Ok);
                        });
                }
                // Aliasing: written through the window, read through the heap's addresses (and back).
                if (err.isOk()) {
                        writePattern(window, 0x77);
                        wrong += checkPattern(run.p, 0x77);
                        writePattern(run.p, 0x99);
                        wrong += checkPattern(window, 0x99);
                        ctx.note("aliasing: %u words wrong (written through one address, read through the other)",
                                 unsigned(wrong));
                }
                // A large range's invalidation really drops what is cached: 64 KB read through the
                // heap's addresses (cached, clean), rewritten through the window and written back,
                // then the heap's whole 8 MB invalidated in one call and the 64 KB read again.
                if (err.isOk()) {
                        size_t stale[2] = {};
                        for (int big = 0; big < 2; ++big) {
                                writePattern(run.p, 0xaa);
                                (void)checkPattern(run.p, 0xaa, Page); // cached now
                                auto *w = reinterpret_cast<uint32_t *>(window);
                                for (size_t i = 0; i < Page / 4; ++i) w[i] = patternWord(i, 0xbb);
                                (void)esp_cache_msync(window, Page,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                                const esp_err_t ierr = esp_cache_msync(run.p, big != 0 ? SpeedBytes : Page,
                                                                       ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                                const auto     *h = reinterpret_cast<const uint32_t *>(run.p);
                                for (size_t i = 0; i < Page / 4; ++i)
                                        stale[big] += h[i] != patternWord(i, 0xbb) ? 1 : 0;
                                if (ierr != ESP_OK) ctx.note("invalidate: %s", esp_err_to_name(ierr));
                        }
                        ctx.note("invalidation: %u words stale after 64 KB invalidated, %u after 8 MB in one call",
                                 unsigned(stale[0]), unsigned(stale[1]));
                }
                // The AXI GDMA: the first half copied onto the second, through each address.
                if (err.isOk()) {
                        async_memcpy_config_t config = {};
                        config.backlog = 8;
                        config.dma_burst_size = 64;
                        async_memcpy_handle_t mcp = nullptr;
                        if (esp_async_memcpy_install_gdma_axi(&config, &mcp) != ESP_OK) return Error::Busy;
                        Semaphore    done{1, 0};
                        const size_t half = SpeedBytes / 2;
                        for (int through = 0; through < 2 && err.isOk(); ++through) {
                                uint8_t *p = through == 0 ? run.p : window;
                                writePattern(p, 0x1234);
                                char name[32];
                                std::snprintf(name, sizeof(name), "%s_dma_copy_mb_s", through == 0 ? "heap" : "window");
                                esp_err_t dmaErr = ESP_OK;
                                err = ctx.measure(name, "MB/s", BenchBetter::Higher, [&](double &v) {
                                        Stopwatch w;
                                        for (size_t off = 0; off < half && dmaErr == ESP_OK; off += 256 * 1024) {
                                                dmaErr = esp_async_memcpy(mcp, p + half + off, p + off, 256 * 1024,
                                                                          dmaDone, &done);
                                                if (dmaErr == ESP_OK) (void)done.acquire(1000);
                                        }
                                        v = mbPerS(half, w.elapsedUs());
                                        return Error(Error::Ok);
                                });
                                if (dmaErr != ESP_OK) {
                                        ctx.note("%s: the GDMA refused the addresses (%s)", name,
                                                 esp_err_to_name(dmaErr));
                                        continue;
                                }
                                // The copy's second half against the pattern's first half, through the heap.
                                invalidate(run.p, SpeedBytes, false);
                                const auto *w = reinterpret_cast<const uint32_t *>(run.p);
                                size_t      bad = 0;
                                for (size_t i = 0; i < half / 4; ++i) bad += w[half / 4 + i] != patternWord(i, 0x1234);
                                ctx.note("%s: %u words copied wrong", name, unsigned(bad));
                        }
                        (void)esp_async_memcpy_uninstall(mcp);
                }
                unmap(window, run.bytes);
                return err;
        }

        // ---- The heap's largest free block: IDF's walk against TLSF's bitmaps

        // multi_heap.c's and tlsf_control_functions.h's private structures, as this IDF has them
        // (a measurement's prototype: a real one gets an accessor checked at build time).
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

        const TlsfControl *psramTlsf() {
                heap_t *h = nullptr;
                SLIST_FOREACH(h, &registered_heaps, next) {
                        if (h->start >= intptr_t(0x48000000) && h->start < intptr_t(0x4c000000))
                                return static_cast<const TlsfControl *>(
                                        reinterpret_cast<const MultiHeap *>(h->heap)->heapData);
                }
                return nullptr;
        }

        int fls(unsigned int word) {
                return word != 0 ? 31 - __builtin_clz(word) : -1;
        }

        // The largest free block's lower bound: its free list's smallest size.
        size_t bitmapLargest(const TlsfControl &c) {
                const int fl = fls(c.flBitmap);
                if (fl <= 0) return 0;
                const int    sl = fls(c.slBitmap[fl]);
                const int    raw = fl + int(c.flIndexShift) - 1;
                const size_t base = size_t(1) << raw;
                return base + (size_t(sl < 0 ? 0 : sl) << (raw - int(c.slIndexCountLog2)));
        }

        Error runLargest(BenchContext &ctx) {
                const TlsfControl *c = psramTlsf();
                if (c == nullptr) return Error::NotFound;
                size_t walked = 0;
                size_t bitmap = 0;
                Error  err = ctx.measure("walk_us", "us", BenchBetter::Lower, [&](double &v) {
                        Stopwatch w;
                        walked = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                        v = double(w.elapsedUs());
                        return Error(Error::Ok);
                });
                if (err.isOk())
                        err = ctx.measure("bitmap_ns", "ns", BenchBetter::Lower, [&](double &v) {
                                Stopwatch w;
                                for (int i = 0; i < 1000; ++i) bitmap = bitmapLargest(*c);
                                v = double(w.elapsedUs());
                                return Error(Error::Ok);
                        });
                multi_heap_info_t info = {};
                heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
                ctx.note("largest free block: %u by the walk, %u or more by the bitmaps (%u lists of %u); %u blocks "
                         "(%u free), %u bytes free",
                         unsigned(walked), unsigned(bitmap), unsigned(c->flIndexCount), unsigned(c->slIndexCount),
                         unsigned(info.total_blocks), unsigned(info.free_blocks), unsigned(info.total_free_bytes));
                return err;
        }

        // ---- A heap cut up on purpose, then a block larger than any of its holes

        constexpr size_t   HoleBytes = 1u << 20;
        constexpr uint32_t MaxHoles = 64;

        Error runFragment(BenchContext &ctx) {
                // 1 MB blocks held on the heap until a few MB are left, then every other one freed:
                // the free memory in 1 MB holes.
                Memory::Options heap;
                heap.heapOnly = true;
                void    *blocks[MaxHoles * 2] = {};
                uint32_t n = 0;
                while (n < MaxHoles * 2 && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 4 * HoleBytes) {
                        blocks[n] = Memory::alloc(MemSpace::Psram, HoleBytes, heap);
                        if (blocks[n] == nullptr) break;
                        ++n;
                }
                for (uint32_t i = 0; i < n; i += 2) {
                        Memory::free(blocks[i]);
                        blocks[i] = nullptr;
                }
                const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                const size_t want = (free / 2) / Page * Page; // half of what is free, in one block
                ctx.note("the heap cut up: %u KB free, its largest block %u KB; asking for %u KB",
                         unsigned(free / 1024), unsigned(largest / 1024), unsigned(want / 1024));
                const Memory::WindowUsage before = Memory::windowUsage();
                void                     *big = nullptr;
                Error                     err = ctx.measure("alloc_us", "us", BenchBetter::Lower, [&](double &v) {
                        Memory::free(big);
                        Stopwatch w;
                        big = Memory::alloc(MemSpace::Psram, want);
                        v = double(w.elapsedUs());
                        return big != nullptr ? Error(Error::Ok) : Error(Error::NoMem);
                });
                const Memory::WindowUsage after = Memory::windowUsage();
                if (big != nullptr) {
                        // Every page written and read back through the one block.
                        auto *w = static_cast<uint32_t *>(big);
                        for (size_t i = 0; i < want / 4; ++i) w[i] = patternWord(i, 0x3c);
                        size_t wrong = 0;
                        for (size_t i = 0; i < want / 4; ++i) wrong += w[i] != patternWord(i, 0x3c) ? 1 : 0;
                        ctx.note("%s, %u runs, %u words wrong",
                                 PsramWindow::contains(big, want) ? "through the window" : "from the heap",
                                 unsigned(after.runs - before.runs), unsigned(wrong));
                }
                Memory::free(big);
                for (uint32_t i = 0; i < n; ++i) Memory::free(blocks[i]);
                if (err.isError()) return err;

                // Grown by realloc from 1 MB to 8 MB a megabyte at a time: in place while the
                // addresses after it are free, its first megabyte kept.
                const uint32_t grownBefore = Memory::windowUsage().grownInPlace;
                auto          *g = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, HoleBytes));
                if (g == nullptr) return Error::NoMem;
                for (size_t i = 0; i < HoleBytes / 4; ++i) g[i] = patternWord(i, 0x5e);
                uint32_t moved = 0;
                for (size_t mb = 2; mb <= 8; ++mb) {
                        auto *r = static_cast<uint32_t *>(Memory::realloc(MemSpace::Psram, g, mb * HoleBytes));
                        if (r == nullptr) break;
                        moved += r != g ? 1 : 0;
                        g = r;
                }
                size_t wrong = 0;
                for (size_t i = 0; i < HoleBytes / 4; ++i) wrong += g[i] != patternWord(i, 0x5e) ? 1 : 0;
                Memory::free(g);
                ctx.note("realloc 1 to 8 MB: %u grown in place, %u moved, %u words wrong",
                         unsigned(Memory::windowUsage().grownInPlace - grownBefore), unsigned(moved), unsigned(wrong));
                return err;
        }

} // namespace

BMDS_BENCH(memoryWindowFragment, "memory.window.fragment", runFragment, BenchQuick,
           "The PSRAM heap cut into 1 MB holes, then one block of half what is free: through the window (us)")
BMDS_BENCH(memoryWindowMapping, "memory.window.mapping", runMapping, BenchQuick,
           "Heap pages mapped into the PSRAM window: the map, unmap, MMU entries, the caches' invalidation (a page at "
           "a time and whole), and a block's allocation and free through Memory, at 64 KB to 16 MB (us)")
BMDS_BENCH(memoryWindowSpeed, "memory.window.speed", runSpeed, BenchQuick,
           "8 MB of PSRAM through window addresses against the heap's own: CPU writes and reads, the AXI GDMA's copy, "
           "and aliasing checked (MB/s)")
BMDS_BENCH(memoryWindowLargest, "memory.window.largest", runLargest, BenchQuick,
           "The PSRAM heap's largest free block: IDF's walk against TLSF's bitmaps (us, ns a thousand)")

BMDS_NAMESPACE_END
