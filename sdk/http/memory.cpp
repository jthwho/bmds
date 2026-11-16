/**
 * @file      memory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds_core's `Memory` in a program (libbmdshttp, devplan/network.md §35d): the program's heap.
 * A program has one kind of memory for this; `MemSpace` says nothing here.
 */

#include <stdlib.h>
#include <bmds/memory.h>

namespace bmds {

        void *Memory::alloc(MemSpace space, size_t sizeBytes) {
                (void)space;
                return malloc(sizeBytes);
        }

        void Memory::free(void *ptr) {
                ::free(ptr);
                return;
        }

} // namespace bmds
