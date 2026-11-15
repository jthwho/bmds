/**
 * @file      usermemory.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_loader: a U-mode program's memory (devplan/protection.md §15).
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/pagewindow.h>
#include <bmds/process.h>
#include <bmds/rangeheap.h>
#include "usermode.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief Everything a U-mode program can reach of memory, and the PMP set that says so
 *        (`devplan/protection.md` §15).
 *
 * - **The main range:** PSRAM window addresses reserved for it, the image's pages first (mapped
 *   by `ImageWindow` from its cache entry, or pages of its own for a plain copy: `backImage()`),
 *   then the heap, mapped up to a top that rises as it needs: its code read and run, the rest read
 *   and write (three PMP entries). When its reservation can't grow in place, more ranges (a pair
 *   each).
 * - **Large blocks** (from `LargeBytes`): a range each, freed whole.
 * - **Internal RAM** (`BMDS_MEM_FAST`, stacks that fit): a region each.
 *
 * Blocks are `RangeHeap`'s: their records are here, none in the program's memory. A heap range is
 * granted up to the highest block made in it (a `GrantStep` at a time), and what is granted is
 * zeroed first: the program sees nothing of the memory's earlier owners, and pages it never uses
 * cost no zeroing. Large blocks are zeroed whole. Large and internal blocks are granted only while a pair of
 * entries stays free for the heap to grow into.
 *
 * Made in PSRAM; its `UserSpace` in internal RAM. Reference-counted: the program (its `Running`,
 * its `Process`) and each surface with buffers here (the compositor reads them at these
 * addresses).
 *
 * @par Thread Safety
 * Thread-safe (a mutex): not from an ISR.
 */
class UserMemory final : public ProgramMemory {
        public:
                /** @brief From this size a block gets a range of its own (`Memory`'s window's rule). */
                static constexpr uint32_t LargeBytes = 256 * 1024;

                /** @brief How far a heap range's grant moves up at a time (zeroed with it). */
                static constexpr uint32_t GrantStep = 4096;

                /**
                 * @brief A program's memory for an image of @p imageBytes, the first @p codeBytes
                 *        of them code and constants (never writable once it runs): addresses
                 *        reserved, nothing mapped. The image goes in next (`ImageWindow` at
                 *        `base()`, or `backImage()`), then `start()`.
                 * @return It (one reference), or null: no addresses, or no memory for it.
                 */
                static UserMemory *create(uint32_t imageBytes, uint32_t codeBytes);

                /** @brief Where the image goes: the main range's start. */
                uint8_t *base() const { return _ranges[0].base; }

                /** @brief Maps pages of its own where the image goes (a plain copy's). */
                Error backImage();

                /**
                 * @brief The image is in place: its regions granted, the rest of its last page
                 *        given to the heap (granted and zeroed as blocks reach it).
                 */
                Error start();

                /** @brief The PMP set its threads run with. */
                UserSpace *space() const { return _space; }

                /** @brief True if @p p is one of its internal RAM blocks. */
                bool isInternal(const void *p);

                /**
                 * @brief A system call of its program begins (`devplan/protection.md` §16).
                 * @return Its epoch: what it checks from now on was granted when the call began.
                 */
                uint32_t enterCall() {
                        // Acquire: a free's revoke (published before it bumped the epoch, release)
                        // is seen by this call's checks if the call saw that epoch.
                        _calls->inFlight.fetch_add(1, std::memory_order_acq_rel);
                        return _calls->epoch.load(std::memory_order_acquire);
                }

                /** @brief The call ends. */
                void leaveCall() {
                        _calls->inFlight.fetch_sub(1, std::memory_order_release);
                        return;
                }

                /** @brief True if freed memory waits for calls that began before it was freed. */
                bool hasDeferred() const { return _calls->deferred.load(std::memory_order_relaxed) != 0; }

                /**
                 * @brief Gives back the freed memory no call in flight may still use: @p oldest
                 *        is the least epoch of the program's calls in flight (0: none).
                 */
                void releaseDeferred(uint32_t oldest);

                /**
                 * @brief Writes @p value to the word at @p p if the program can still write it
                 *        (a word the kernel kept: the signal hint, `thread_clear`'s), under the
                 *        lock that freeing takes. @return False if it couldn't.
                 */
                bool poke(volatile uint32_t *p, uint32_t value);

                /** @brief As `poke()`, from a call of the program in flight: nothing it can reach goes meanwhile. */
                bool pokeInCall(volatile uint32_t *p, uint32_t value) {
                        if (!isAccessible(const_cast<const uint32_t *>(p), sizeof(uint32_t), true)) return false;
                        *p = value;
                        return true;
                }

                /** @brief What `mem` and the debug log show. */
                struct Usage {
                                uint32_t mappedPages =
                                        0; ///< PSRAM pages of its own mapped (the image's shared ones not).
                                uint32_t reservedPages = 0; ///< Window pages its ranges hold.
                                uint32_t internalBytes = 0; ///< Internal RAM blocks.
                                uint32_t heapUsed = 0;      ///< The heap's blocks.
                                uint32_t heapFree = 0;      ///< Free in its mapped pages.
                                uint8_t  ranges = 0;        ///< Heap ranges.
                                uint8_t  large = 0;         ///< Large blocks.
                                uint8_t  internal = 0;      ///< Internal blocks.
                                uint8_t  freeEntries = 0;   ///< PMP entries free.
                };

                /** @brief Returns the counts. */
                Usage usage();

                // ProgramMemory
                void     ref() override;
                void     unref() override;
                void    *alloc(uint32_t bytes, uint32_t align, uint32_t flags, uint32_t owner) override;
                uint32_t free(void *p, uint32_t owner) override;
                void    *realloc(void *p, uint32_t bytes, uint32_t flags, uint32_t owner) override;
                uint32_t sizeOf(const void *p, uint32_t owner) override;
                bool     contains(const void *p, size_t bytes, uint32_t owner) override;
                void     freeOwner(uint32_t owner) override;
                size_t   accessible(const void *p, bool write) override;

        private:
                static constexpr unsigned MaxRanges = 4; // the heap's
                static constexpr unsigned MaxLarge = 6;
                static constexpr unsigned MaxInternal = 6;
                static constexpr unsigned KeepFree = 2; // entries for the heap's growth

                // A run of heap pages mapped at a range's page `page`.
                struct Run {
                                uint8_t *at;
                                uint32_t pages;
                                uint32_t page;
                };

                // Window addresses: `reserved` pages from `base`, the first `mapped` mapped (the
                // image's `imagePages` by ImageWindow, the rest by `runs`).
                struct Range {
                                uint8_t  *base = nullptr;
                                uint32_t  reserved = 0;
                                uint32_t  mapped = 0;
                                uint32_t  imagePages = 0;
                                uintptr_t region = 0;      // its writable PMP region's base
                                uintptr_t grantTop = 0;    // a heap range's: granted (and zeroed) below this
                                bool      granted = false; // the region is in the PMP set
                                List<Run> runs;
                };

                // A freed one waits (`deferred`: the epoch it was freed in) while older calls run.
                struct Large {
                                Range    range;
                                uint32_t bytes = 0;
                                uint32_t owner = 0;
                                uint32_t deferred = 0;
                };

                struct Internal {
                                void    *block = nullptr;
                                uint32_t bytes = 0;
                                uint32_t owner = 0;
                                uint32_t deferred = 0;
                };

                UserMemory() = default;
                ~UserMemory();

                Error     map(Range &r, uint32_t pages, bool zero);
                void      unmapTail(Range &r, uint32_t pages);
                void      release(Range &r);
                Error     growHeap(uint32_t bytes);
                Error     raiseGrant(Range &r, uintptr_t end);
                void      trim(const RangeHeap::Freed &f);
                Range    *heapRangeOf(uint32_t at);
                void     *allocLarge(uint32_t bytes, uint32_t owner);
                Large    *largeAt(const void *p);
                void      freeLarge(Large &l);
                bool      resizeLarge(Large &l, uint32_t bytes);
                void     *allocInternal(uint32_t bytes, uint32_t align, uint32_t flags, uint32_t owner);
                Internal *internalAt(const void *p);
                void      freeInternal(Internal &b);
                void     *heapAlloc(uint32_t bytes, uint32_t align, uint32_t owner);
                uint32_t  blockSize(const void *p, uint32_t owner);
                void      freeLocked(void *p, uint32_t owner, uint32_t &bytes);

                std::atomic<uint32_t> _refs{1};
                UserSpace::Calls     *_calls = nullptr; // the space's (internal RAM)
                Mutex                 _lock;
                UserSpace            *_space = nullptr;
                RangeHeap             _heap;
                Range                 _ranges[MaxRanges];
                unsigned              _rangeCount = 0;
                Large                 _large[MaxLarge];
                Internal              _internal[MaxInternal];
                uint32_t              _imageBytes = 0;
                uint32_t              _codeBytes = 0;
};

BMDS_NAMESPACE_END
