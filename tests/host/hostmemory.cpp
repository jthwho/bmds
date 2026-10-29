/**
 * @file      hostmemory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "hostmemory.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <bmds/memory.h>

namespace {

        // Atomic: pipeline stages allocate on worker threads.
        std::atomic<size_t> live{0};
        std::atomic<long>   remaining{-1};

        std::atomic<bmds::Memory::Reclaimer *> reclaimers[bmds::Memory::MaxReclaimers] = {};

        // As the target's: each in turn, until what was asked is freed.
        bool reclaimed(bmds::MemSpace space, size_t sizeBytes) {
                if (space != bmds::MemSpace::Psram && space != bmds::MemSpace::PsramDma) return false;
                size_t freed = 0;
                for (auto &slot : reclaimers) {
                        bmds::Memory::Reclaimer *r = slot.load();
                        if (r != nullptr)
                                freed += r->reclaim(space, sizeBytes - (freed < sizeBytes ? freed : sizeBytes));
                        if (freed >= sizeBytes) break;
                }
                return freed != 0;
        }

        bool mayAllocate() {
                long r = remaining.load();
                for (;;) {
                        if (r < 0) return true;
                        if (r == 0) return false;
                        if (remaining.compare_exchange_weak(r, r - 1)) return true;
                }
        }

        void *counted(void *p) {
                if (p != nullptr) ++live;
                return p;
        }

} // namespace

size_t HostMemory::liveAllocations() {
        return live;
}

void HostMemory::failAfter(long successes) {
        remaining = successes;
        return;
}

// A failure injected by failAfter() asks the reclaimer, as the target's would.
void *bmds::Memory::alloc(MemSpace space, size_t sizeBytes) {
        if (mayAllocate() || (reclaimed(space, sizeBytes) && mayAllocate())) return counted(std::malloc(sizeBytes));
        return nullptr;
}

void *bmds::Memory::allocAligned(MemSpace space, size_t sizeBytes, size_t alignBytes) {
        if (!mayAllocate() && !(reclaimed(space, sizeBytes) && mayAllocate())) return nullptr;
        // aligned_alloc wants the size to be a multiple of the alignment.
        return counted(std::aligned_alloc(alignBytes, (sizeBytes + alignBytes - 1) & ~(alignBytes - 1)));
}

void *bmds::Memory::alloc(MemSpace space, size_t sizeBytes, const Options &options) {
        return options.alignBytes != 0 ? allocAligned(space, sizeBytes, options.alignBytes) : alloc(space, sizeBytes);
}

// The host's heap has no window: a new block and a copy, as the target's realloc of a heap block.
void *bmds::Memory::realloc(MemSpace space, void *ptr, size_t sizeBytes, size_t alignBytes) {
        Options o;
        o.alignBytes = alignBytes;
        if (ptr == nullptr) return alloc(space, sizeBytes, o);
        if (sizeBytes == 0) {
                free(ptr);
                return nullptr;
        }
        const size_t old = malloc_usable_size(ptr);
        void        *grown = alloc(space, sizeBytes, o);
        if (grown == nullptr) return nullptr;
        std::memcpy(grown, ptr, old < sizeBytes ? old : sizeBytes);
        free(ptr);
        return grown;
}

bmds::Memory::WindowUsage bmds::Memory::windowUsage() {
        return WindowUsage();
}

bmds::Memory::WindowHistory bmds::Memory::windowHistory() {
        return WindowHistory();
}

void bmds::Memory::clearWindowHistory() {
        return;
}

void bmds::Memory::free(void *ptr) {
        if (ptr == nullptr) return;
        --live;
        std::free(ptr);
        return;
}

bmds::Error bmds::Memory::addReclaimer(Reclaimer *r) {
        if (r == nullptr) return Error::Invalid;
        for (auto &slot : reclaimers)
                if (slot.load() == r) return Error::Invalid;
        for (auto &slot : reclaimers) {
                Reclaimer *none = nullptr;
                if (slot.compare_exchange_strong(none, r)) return Error::Ok;
        }
        return Error::Full;
}

void bmds::Memory::removeReclaimer(Reclaimer *r) {
        for (auto &slot : reclaimers) {
                Reclaimer *was = r;
                (void)slot.compare_exchange_strong(was, nullptr);
        }
        return;
}

size_t bmds::Memory::reclaimerUsage(Span<Reclaimer::Usage> out) {
        size_t n = 0;
        for (auto &slot : reclaimers) {
                Reclaimer *r = slot.load();
                if (r == nullptr) continue;
                if (n < out.size()) out[n] = r->usage();
                ++n;
        }
        return n;
}

bmds::MemStats bmds::Memory::stats(MemSpace) {
        return MemStats();
}
