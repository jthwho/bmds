/**
 * @file      memory.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
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
                 * @brief Memory held for later that can be given back when an
                 *        allocation fails (the loader's image cache).
                 *
                 * `reclaim()` runs inside the failed allocation, in the allocating
                 * task: it must not allocate, and nothing that holds a lock it takes
                 * may allocate.
                 */
                class Reclaimer {
                        public:
                                /** @brief What it holds, and how it has done (`mem`). */
                                struct Usage {
                                                const char *items = "";      ///< What it holds, plural ("programs").
                                                size_t      bytes = 0;       ///< Bytes it holds.
                                                size_t      count = 0;       ///< Items it holds.
                                                size_t      reclaimable = 0; ///< Bytes `reclaim()` could free now.
                                                uint32_t    hits = 0;        ///< Lookups that found an item.
                                                uint32_t    misses = 0;      ///< Lookups that didn't.
                                                uint32_t    reclaims = 0; ///< `reclaim()` calls that freed something.
                                };

                                /**
                                 * @brief Frees what it holds in @p space, at least @p bytes
                                 *        if it can.
                                 * @return The bytes freed.
                                 */
                                virtual size_t reclaim(MemSpace space, size_t bytes) = 0;

                                /** @brief Returns what it holds now, and its counts since boot. */
                                virtual Usage usage() const = 0;

                        protected:
                                ~Reclaimer() = default;
                };

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

                /**
                 * @brief Installs @p reclaimer (null: none): a `Psram` or `PsramDma`
                 *        allocation that fails asks it, then tries once more if it
                 *        freed anything.
                 * @param reclaimer Lives as long as it is installed.
                 */
                static void setReclaimer(Reclaimer *reclaimer);

                /**
                 * @brief Returns the reclaimer's usage into @p usage.
                 * @return False if none is installed.
                 */
                static bool reclaimerUsage(Reclaimer::Usage &usage);
};

BMDS_NAMESPACE_END
