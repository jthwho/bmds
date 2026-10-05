/**
 * @file      memory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <esp_heap_caps.h>
#include <bmds/board.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t capsFor(MemSpace space) {
                return board().memSpace(space).heapCaps;
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
        return heap_caps_malloc(dmaSize(space, sizeBytes), capsFor(space));
}

void *Memory::allocAligned(MemSpace space, size_t sizeBytes, size_t alignBytes) {
        return heap_caps_aligned_alloc(alignBytes, dmaSize(space, sizeBytes), capsFor(space));
}

void Memory::free(void *ptr) {
        heap_caps_free(ptr);
        return;
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
