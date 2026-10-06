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
#include <bmds/memory.h>

namespace {

        // Atomic: pipeline stages allocate on worker threads.
        std::atomic<size_t> live{0};
        std::atomic<long>   remaining{-1};

        std::atomic<bmds::Memory::Reclaimer *> reclaimer{nullptr};

        bool reclaimed(bmds::MemSpace space, size_t sizeBytes) {
                if (space != bmds::MemSpace::Psram && space != bmds::MemSpace::PsramDma) return false;
                bmds::Memory::Reclaimer *r = reclaimer.load();
                return r != nullptr && r->reclaim(space, sizeBytes) != 0;
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

void bmds::Memory::free(void *ptr) {
        if (ptr == nullptr) return;
        --live;
        std::free(ptr);
        return;
}

void bmds::Memory::setReclaimer(Reclaimer *r) {
        reclaimer.store(r);
        return;
}

bool bmds::Memory::reclaimerUsage(Reclaimer::Usage &usage) {
        Reclaimer *r = reclaimer.load();
        if (r == nullptr) return false;
        usage = r->usage();
        return true;
}

bmds::MemStats bmds::Memory::stats(MemSpace) {
        return MemStats();
}
