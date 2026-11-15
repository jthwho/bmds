/**
 * @file      rangeheap.h
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
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An allocator of address ranges whose records are all its own, none in the memory it
 *        hands out (`devplan/protection.md` §15): a U-mode program's heap, where a header in
 *        front of each block would be the program's to rewrite.
 *
 * It manages addresses only: the caller adds ranges (`addRange()`) and gives each block's memory
 * its meaning. Blocks are `Unit`-aligned multiples of `Unit`, found TLSF's way (size classes and a
 * bitmap: constant time), each record linked to its neighbours for merging, and looked up by its
 * start in a hash. Each block has an owner, so one heap can serve several (a process, its vfork
 * child, the kernel) and free one's blocks at once.
 *
 * Records come from `Memory` (`Psram`); a call that needs one and can't get it fails with
 * `NoMem`, nothing changed.
 *
 * @par Thread Safety
 * Not thread-safe: its owner's lock.
 */
class RangeHeap {
        public:
                /** @brief The granularity: every block's start and size are multiples of it. */
                static constexpr uint32_t Unit = 16;

                /** @brief The largest alignment `alloc()` takes. */
                static constexpr uint32_t MaxAlign = 64 * 1024;

                /** @brief What `free()` did. */
                struct Freed {
                                uint32_t bytes = 0;     ///< The block's size (0: not a block of that owner).
                                uint32_t freeAt = 0;    ///< The free block it is now part of, merged.
                                uint32_t freeBytes = 0; ///< Its size.
                };

                /** @brief Counts. */
                struct Stats {
                                uint32_t usedBytes = 0;   ///< In blocks.
                                uint32_t freeBytes = 0;   ///< Free in the ranges.
                                uint32_t blocks = 0;      ///< Blocks allocated.
                                uint32_t largestFree = 0; ///< The largest free block (a lower bound of what fits).
                };

                RangeHeap();
                ~RangeHeap();
                RangeHeap(const RangeHeap &) = delete;
                RangeHeap &operator=(const RangeHeap &) = delete;

                /**
                 * @brief Adds [@p at, @p at + @p bytes) as free space, merged with a free block
                 *        that ends at @p at.
                 * @return `Ok`, `Invalid` (not `Unit` multiples, empty, or past 4 GB), `NoMem`.
                 */
                Error addRange(uint32_t at, uint32_t bytes);

                /**
                 * @brief A block of @p bytes (rounded up to `Unit`; 0 as 1) aligned to @p align (a
                 *        power of two, `Unit` at least, `MaxAlign` at most) for @p owner (any value
                 *        but `UINT32_MAX` and `UINT32_MAX - 1`).
                 * @return Its start; `NoMem` (no free block that big, or no record), `Invalid`.
                 */
                Result<uint32_t> alloc(uint32_t bytes, uint32_t align, uint32_t owner);

                /** @brief Frees @p owner's block at @p at, merged with free neighbours. */
                Freed free(uint32_t at, uint32_t owner);

                /** @brief Frees every block of @p owner. */
                void freeOwner(uint32_t owner);

                /**
                 * @brief Resizes @p owner's block at @p at to @p bytes in place: shrinking, or
                 *        growing into the free block after it.
                 * @return `Ok`; `NoMem` (no room after it, or no record), `NotExist`.
                 */
                Error resize(uint32_t at, uint32_t bytes, uint32_t owner);

                /** @brief The size of @p owner's block at @p at, or 0. */
                uint32_t sizeOf(uint32_t at, uint32_t owner) const;

                /**
                 * @brief True if [@p at, @p at + @p bytes) lies in one of @p owner's blocks. A walk
                 *        of the records: for checks made once.
                 */
                bool contains(uint32_t at, uint32_t bytes, uint32_t owner) const;

                /**
                 * @brief Takes the last @p bytes of the free block at @p freeAt (from `free()`)
                 *        out of the heap: the range's end given back.
                 * @return `Ok`, or `NotExist` (not a free block that long).
                 */
                Error removeEnd(uint32_t freeAt, uint32_t bytes);

                /** @brief Returns the counts. */
                Stats stats() const;

        private:
                static constexpr uint32_t None = UINT32_MAX;
                static constexpr uint32_t FreeOwner = UINT32_MAX;      // a free block's owner
                static constexpr uint32_t SpareOwner = UINT32_MAX - 1; // a record not in use
                static constexpr unsigned SlBits = 2;                  // four classes a power of two
                static constexpr unsigned SlCount = 1u << SlBits;
                static constexpr unsigned FlShift = 4; // log2(Unit)
                static constexpr unsigned FlCount = 32 - FlShift;

                struct Node {
                                uint32_t at;
                                uint32_t size;
                                uint32_t prev;     // the block just below, if adjacent
                                uint32_t next;     // the block just above, if adjacent
                                uint32_t freePrev; // a free block's list; a spare record's next spare
                                uint32_t freeNext;
                                uint32_t hashNext; // the hash's chain (free and used)
                                uint32_t owner;
                };

                Error    reserve(uint32_t records);
                uint32_t takeRecord();
                void     dropRecord(uint32_t i);
                Error    growHash();
                uint32_t bucketOf(uint32_t at) const;
                void     hashInsert(uint32_t i);
                void     hashRemove(uint32_t i);
                uint32_t find(uint32_t at) const;
                void     listInsert(uint32_t i);
                void     listRemove(uint32_t i);
                uint32_t findFree(uint32_t bytes) const;
                uint32_t merge(uint32_t i);
                uint32_t split(uint32_t i, uint32_t bytes);

                Node     *_nodes = nullptr;
                uint32_t  _capacity = 0;
                uint32_t  _spare = None; // the spare records' list
                uint32_t  _spares = 0;
                uint32_t *_buckets = nullptr;
                uint32_t  _bucketCount = 0;
                uint32_t  _hashShift = 32;
                uint32_t  _hashed = 0;
                uint32_t  _flBitmap = 0;
                uint32_t  _slBitmap[FlCount] = {};
                uint32_t  _lists[FlCount][SlCount];
                uint32_t  _usedBytes = 0;
                uint32_t  _freeBytes = 0;
                uint32_t  _blocks = 0;
};

BMDS_NAMESPACE_END
