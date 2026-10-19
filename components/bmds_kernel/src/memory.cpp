/**
 * @file      memory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <bmds/board.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>

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

} // namespace

void *Memory::alloc(MemSpace space, size_t sizeBytes) {
        void *p = heap_caps_malloc(dmaSize(space, sizeBytes), capsFor(space));
        if (p == nullptr && reclaimed(space, sizeBytes))
                p = heap_caps_malloc(dmaSize(space, sizeBytes), capsFor(space));
        return p;
}

void *Memory::allocAligned(MemSpace space, size_t sizeBytes, size_t alignBytes) {
        void *p = heap_caps_aligned_alloc(alignBytes, dmaSize(space, sizeBytes), capsFor(space));
        if (p == nullptr && reclaimed(space, sizeBytes + alignBytes))
                p = heap_caps_aligned_alloc(alignBytes, dmaSize(space, sizeBytes), capsFor(space));
        return p;
}

void Memory::free(void *ptr) {
        heap_caps_free(ptr);
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

namespace {

        constinit std::atomic<uintptr_t> programWindowStart{0};
        constinit std::atomic<size_t>    programWindowBytes{0};

} // namespace

void Memory::setProgramWindow(const void *start, size_t sizeBytes) {
        programWindowBytes.store(sizeBytes, std::memory_order_relaxed);
        programWindowStart.store(reinterpret_cast<uintptr_t>(start), std::memory_order_release);
        return;
}

bool Memory::isExternal(const void *p, size_t sizeBytes) {
        if (esp_ptr_external_ram(p) &&
            (sizeBytes == 0 || esp_ptr_external_ram(static_cast<const uint8_t *>(p) + sizeBytes - 1)))
                return true;
        const uintptr_t start = programWindowStart.load(std::memory_order_acquire);
        const uintptr_t at = reinterpret_cast<uintptr_t>(p);
        return start != 0 && at >= start && at + sizeBytes >= at &&
               at + sizeBytes <= start + programWindowBytes.load(std::memory_order_relaxed);
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
        return s;
}

BMDS_NAMESPACE_END
