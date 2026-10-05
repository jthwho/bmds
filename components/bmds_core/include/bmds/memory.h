/**
 * @file      memory.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/** @brief Usage statistics for one MemSpace. */
struct MemStats {
                size_t totalBytes = 0;       ///< Heap capacity of the space.
                size_t freeBytes = 0;        ///< Currently free.
                size_t minFreeBytes = 0;     ///< Low-water mark of `freeBytes` since boot.
                size_t largestFreeBlock = 0; ///< Largest single allocation that could succeed now.
};

/**
 * @brief Raw MemSpace-aware allocation.
 *
 * The primitive under `Buffer` and the containers (`String`, `List`,
 * `Arena`). Prefer those (or RAII owners) over calling this directly.
 *
 * Declared in `bmds_core` and defined by the platform, like
 * `Detail::assertFailed()`: the kernel on the target (heap capabilities
 * from the board definition), the test harness on the host (malloc, with
 * failure injection).
 *
 * Spaces overlap physically (e.g. `InternalDma` is
 * part of `Internal`), so their statistics are not additive.
 *
 * Allocation is not permitted in ISRs or on hot paths.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
class Memory {
        public:
                /**
                 * @brief Allocates from a memory space.
                 *
                 * DMA spaces are aligned for DMA and cache maintenance
                 * automatically, and get whole cache lines plus one more: no
                 * heap bookkeeping shares a cache line with a DMA buffer.
                 *
                 * @param space     The memory space.
                 * @param sizeBytes Number of bytes.
                 * @return The allocation, or null if out of memory (a normal,
                 *         recoverable condition: report `Error::NoMem`).
                 */
                static void *alloc(MemSpace space, size_t sizeBytes);

                /**
                 * @brief Allocates from a memory space with explicit alignment.
                 * @param space      The memory space.
                 * @param sizeBytes  Number of bytes.
                 * @param alignBytes Alignment, a power of two.
                 * @return The allocation, or null if out of memory.
                 */
                static void *allocAligned(MemSpace space, size_t sizeBytes, size_t alignBytes);

                /**
                 * @brief Frees memory from `alloc()` or `allocAligned()`.
                 * @param ptr The allocation; null is ignored.
                 */
                static void free(void *ptr);

                /** @brief Returns current usage statistics for @p space. */
                static MemStats stats(MemSpace space);
};

BMDS_NAMESPACE_END
