/**
 * @file      fakememory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The memory system calls on the host: aligned malloc, counted.
 */

#include "fakememory.h"

#include <atomic>
#include <cstdlib>

namespace {

        std::atomic<size_t> allocations{0};

        void *memAlloc(uint32_t size, uint32_t align, uint32_t flags) {
                if ((flags & ~BMDS_MEM_FAST) != 0) return nullptr;
                if (align == 0) align = 16;
                if ((align & (align - 1)) != 0) return nullptr;
                if (align < sizeof(void *)) align = sizeof(void *);
                void *p = nullptr;
                if (posix_memalign(&p, align, size != 0 ? size : 1) != 0) return nullptr;
                ++allocations;
                return p;
        }

        void memFree(void *ptr) {
                if (ptr == nullptr) return;
                --allocations;
                std::free(ptr);
                return;
        }

} // namespace

namespace FakeMemory {

        void install(bmds_sys_fn_t *slots) {
                slots[BMDS_SYS_MEM_ALLOC] = reinterpret_cast<bmds_sys_fn_t>(memAlloc);
                slots[BMDS_SYS_MEM_FREE] = reinterpret_cast<bmds_sys_fn_t>(memFree);
                return;
        }

        size_t live() {
                return allocations.load();
        }

} // namespace FakeMemory
