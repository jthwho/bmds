/**
 * @file      memory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <esp_heap_caps.h>
#include <bmds/board.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t capsFor(MemSpace space) {
                return board().memSpace(space).heapCaps;
        }

        constinit std::atomic<Memory::Reclaimer *> reclaimers[Memory::MaxReclaimers] = {};

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

        // The largest cache line (L2, in front of the PSRAM).
        constexpr size_t CacheLine = 128;

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
        for (auto &slot : reclaimers) {
                Reclaimer *none = nullptr;
                if (slot.compare_exchange_strong(none, r)) return Error::Ok;
        }
        return Error::Full;
}

void Memory::removeReclaimer(Reclaimer *r) {
        for (auto &slot : reclaimers) {
                Reclaimer *was = r;
                (void)slot.compare_exchange_strong(was, nullptr);
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
