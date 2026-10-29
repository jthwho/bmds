/**
 * @file      psramwindow.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief The PSRAM addresses the heap doesn't use, where heap pages are mapped again as one range
 *        (`devplan/psramwindow.md` §3a): `Memory`'s large blocks and programs' images
 *        (`ImageWindow`) both take their ranges here.
 *
 * IDF's heap maps the 32 MB of PSRAM in order at `0x48000000`; the MMU has entries for another
 * 32 MB of addresses after it, all reserved here at boot. A range's pages point to physical pages
 * the heap gave; the caller keeps the caches right:
 * - before mapping, the pages' lines under their heap addresses dropped (`invalidate()`);
 * - after mapping, the range's stale lines dropped;
 * - before unmapping, the range's lines dropped (written back, they would go nowhere).
 *
 * The caches invalidate a range in about 7 µs whatever its size, and entries are written
 * directly (IDF's `mmu_ll_write_entry()` reads an eFuse three times an entry): about 6 µs a
 * megabyte to map (§2a).
 *
 * @par Thread Safety
 * Thread-safe; `contains()` is ISR-safe. Ranges under the window's lock (blocking), entries under
 * the MMU's spinlock.
 */
namespace PsramWindow {

        /** @brief The MMU's page. */
        constexpr size_t PageBytes = 64 * 1024;

        /**
         * @brief Boot stage: reserves the free PSRAM addresses (every entry invalid) and checks
         *        that the heap's pages are mapped in order.
         * @return `Ok`, or the error reserving them (the window then stays off: everything comes
         *         from the heap, as before).
         */
        Error init();

        /** @brief True once `init()` reserved the addresses. */
        bool isReady();

        /** @brief True if @p bytes at @p p lie in the window. @isrsafe */
        bool contains(const void *p, size_t bytes);

        /** @brief Returns the window's first address (null before `init()`). */
        uint8_t *base();

        /** @brief Returns its pages. */
        uint32_t pages();

        /**
         * @brief Takes a range of @p pages pages: the lowest that fits, or with @p fromTop the
         *        highest (blocks kept for the system's life, so they don't sit between ranges that
         *        come and go).
         * @return Its first address, or null (no range that long).
         * @blocking The window's lock.
         */
        uint8_t *takeRange(uint32_t pages, bool fromTop);

        /** @brief Takes the @p pages pages at @p at if they are all free (growing in place). */
        bool takeRangeAt(uint8_t *at, uint32_t pages);

        /** @brief Gives back a range (unmapped first: `clear()`). */
        void giveRange(uint8_t *at, uint32_t pages);

        /** @brief What `mem` shows of the window's addresses. */
        struct Usage {
                        uint32_t pages = 0;   ///< Its pages.
                        uint32_t used = 0;    ///< In ranges.
                        uint32_t longest = 0; ///< The longest free range.
        };

        /** @brief Returns the counts. */
        Usage usage();

        /**
         * @brief Points @p pages pages from @p at to the physical pages behind @p heap and the
         *        pages after it: a block of whole pages from the heap (its pages in order), or
         *        from the window itself (each page's own).
         */
        void mapHeap(uint8_t *at, const uint8_t *heap, uint32_t pages);

        /** @brief Points the page at @p at to the physical page behind heap address @p heap. */
        void mapPage(uint8_t *at, const uint8_t *heap);

        /** @brief Makes the @p pages pages from @p at invalid. */
        void clear(uint8_t *at, uint32_t pages);

        /** @brief Returns the physical page behind PSRAM address @p p (the heap's or the window's). */
        uint32_t physicalPage(const void *p);

        /**
         * @brief Drops every cache's lines of @p bytes at @p p (both a cache line's multiple) in
         *        one call: about 7 µs at any size.
         */
        void invalidate(const void *p, size_t bytes);

} // namespace PsramWindow

BMDS_NAMESPACE_END
