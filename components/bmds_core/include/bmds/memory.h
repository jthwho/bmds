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
#include <bmds/error.h>
#include <bmds/span.h>
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
 * Large PSRAM blocks may come from the PSRAM window (`devplan/psramwindow.md`): heap pages
 * gathered wherever they are and mapped by the MMU as one range, so a cut-up heap doesn't fail
 * them. Callers see a contiguous block either way; `Options::heapOnly` keeps a block on the heap
 * for code that checks addresses against the heap's range.
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
                 * @brief The largest cache line of any configuration (the P4's L2, in
                 *        front of the PSRAM: 64 or 128 bytes): a DMA buffer aligned to it
                 *        in address and size is aligned for every cache (devplan/bench.md
                 *        §19).
                 */
                static constexpr size_t DmaAlign = 128;

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

                /** @brief How long a block is kept. */
                enum class Lifetime : uint8_t {
                        Normal, ///< Freed when done with.
                        System, ///< Kept for the system's life (surface 0's buffers): placed apart.
                };

                /** @brief How `alloc()` places a block. */
                struct Options {
                                size_t   alignBytes = 0; ///< A power of two; 0: the space's own.
                                Lifetime lifetime = Lifetime::Normal;
                                bool     heapOnly = false; ///< Never through the window: its address is
                                                           ///< checked against the heap's (a task's stack).
                };

                /** @brief What the PSRAM window holds, and how the policy has chosen (`mem`). */
                struct WindowUsage {
                                uint32_t pages = 0;        ///< The window's pages (0: off).
                                uint32_t used = 0;         ///< In use (blocks and programs' images).
                                uint32_t longest = 0;      ///< The longest free range of pages.
                                uint32_t blocks = 0;       ///< `Memory`'s blocks mapped now.
                                uint32_t runs = 0;         ///< Their heap runs.
                                uint64_t bytes = 0;        ///< What they hold.
                                uint32_t byHeap = 0;       ///< Large requests the policy sent to the heap.
                                uint32_t bySize = 0;       ///< Sent to the window: large enough always.
                                uint32_t byFit = 0;        ///< ...: the heap's largest block too small.
                                uint32_t byCutUp = 0;      ///< ...: the heap cut up.
                                uint32_t byReserve = 0;    ///< ...: the heap's contiguous reserve kept.
                                uint32_t toWindow = 0;     ///< Heap failures the window served.
                                uint32_t toHeap = 0;       ///< Window failures the heap served.
                                uint32_t grownInPlace = 0; ///< `realloc()`s grown without a copy.
                };

                /**
                 * @brief How the policy has chosen across resets (`mem`): kept where a reset leaves
                 *        it (only power-up, a new firmware or `clearWindowHistory()` starts it again).
                 */
                struct WindowHistory {
                                uint32_t boots = 0;         ///< Boots counted, this one included (0: none kept).
                                uint64_t seconds = 0;       ///< Since it started.
                                uint32_t byHeap = 0;        ///< As `WindowUsage`'s, summed over the boots.
                                uint32_t bySize = 0;        ///< ...
                                uint32_t byFit = 0;         ///< ...
                                uint32_t byCutUp = 0;       ///< ...
                                uint32_t byReserve = 0;     ///< ...
                                uint32_t toWindow = 0;      ///< ...
                                uint32_t toHeap = 0;        ///< ...
                                uint32_t grownInPlace = 0;  ///< ...
                                uint32_t failed = 0;        ///< Large requests nothing could give.
                                uint32_t peakUsed = 0;      ///< The most window pages in use at once.
                                uint32_t peakBlocks = 0;    ///< The most `Memory` blocks in it at once.
                                uint32_t mostRuns = 0;      ///< The most heap runs one block took.
                                uint64_t lowestLargest = 0; ///< The heap's smallest largest free block the policy saw.
                                uint64_t freeThen = 0;      ///< The heap's free bytes then.
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
                 * @brief Allocates with @p options (alignment, lifetime, heap only).
                 * @return The allocation, or null if out of memory.
                 */
                static void *alloc(MemSpace space, size_t sizeBytes, const Options &options);

                /**
                 * @brief Resizes a block from `alloc()` (null: allocates), keeping its contents up
                 *        to the smaller size. A window block grows in place when the addresses
                 *        after it are free; otherwise a new block in @p space (aligned to
                 *        @p alignBytes, 0: the space's own) and one copy.
                 * @return The block (perhaps moved), or null with @p ptr untouched.
                 */
                static void *realloc(MemSpace space, void *ptr, size_t sizeBytes, size_t alignBytes = 0);

                /**
                 * @brief Frees memory from `alloc()` or `allocAligned()`.
                 * @param ptr The allocation; null is ignored.
                 */
                static void free(void *ptr);

                /** @brief Returns current usage statistics for @p space. */
                static MemStats stats(MemSpace space);

                /** @brief Returns the PSRAM window's use and the policy's choices. */
                static WindowUsage windowUsage();

                /** @brief Returns the policy's choices across resets. */
                static WindowHistory windowHistory();

                /** @brief Starts the history again (from this boot). */
                static void clearWindowHistory();

                /**
                 * @brief True if @p sizeBytes at @p p lie in PSRAM: the heap's, or the window's
                 *        (large blocks, programs' pages), which IDF's `esp_ptr_external_ram()`
                 *        doesn't know. For deciding whether a DMA can reach a buffer.
                 * @isrsafe
                 */
                static bool isExternal(const void *p, size_t sizeBytes);

                /** @brief Reclaimers installed at once at most. */
                static constexpr size_t MaxReclaimers = 4;

                /**
                 * @brief Installs @p reclaimer: a `Psram` or `PsramDma` allocation that
                 *        fails asks each installed one in turn, in the order they were
                 *        installed, until enough is freed, then tries once more if any
                 *        freed something.
                 * @param reclaimer Lives as long as it is installed.
                 * @return `Ok`, `Full`, `Invalid` (null, or installed already).
                 */
                static Error addReclaimer(Reclaimer *reclaimer);

                /** @brief Uninstalls @p reclaimer (nothing if it isn't installed). */
                static void removeReclaimer(Reclaimer *reclaimer);

                /**
                 * @brief Fills @p out with the installed reclaimers' usage, in order.
                 * @return How many are installed.
                 */
                static size_t reclaimerUsage(Span<Reclaimer::Usage> out);
};

BMDS_NAMESPACE_END
