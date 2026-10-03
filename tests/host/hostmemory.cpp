/**
 * @file      hostmemory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "hostmemory.h"

#include <cstdlib>
#include <bmds/memory.h>

namespace {

        size_t live = 0;
        long   remaining = -1;

        bool mayAllocate() {
                if (remaining < 0) return true;
                if (remaining == 0) return false;
                --remaining;
                return true;
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

void *bmds::Memory::alloc(MemSpace, size_t sizeBytes) {
        return mayAllocate() ? counted(std::malloc(sizeBytes)) : nullptr;
}

void *bmds::Memory::allocAligned(MemSpace, size_t sizeBytes, size_t alignBytes) {
        if (!mayAllocate()) return nullptr;
        // aligned_alloc wants the size to be a multiple of the alignment.
        return counted(std::aligned_alloc(alignBytes, (sizeBytes + alignBytes - 1) & ~(alignBytes - 1)));
}

void bmds::Memory::free(void *ptr) {
        if (ptr == nullptr) return;
        --live;
        std::free(ptr);
        return;
}

bmds::MemStats bmds::Memory::stats(MemSpace) {
        return MemStats();
}
