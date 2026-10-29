/**
 * @file      pagewindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The bookkeeping of a window of MMU pages (`devplan/psramwindow.md` §3a): which pages of
 *        a range of addresses are in use, ranges taken first fit from the bottom or last fit
 *        from the top, and the longest free range kept up to date.
 *
 * Portable: no MMU, no heap. `PsramWindow` (the kernel) keeps one for the PSRAM addresses the
 * heap doesn't use, and maps the pages it hands out.
 *
 * @par Thread Safety
 * Not thread-safe: its owner's lock.
 */
class PageWindow {
        public:
                /** @brief The most pages a window has (32 MB of 64 KB pages). */
                static constexpr uint32_t MaxPages = 512;

                /** @brief No range (`take()`'s failure). */
                static constexpr uint32_t None = 0xffffffffu;

                /** @brief Empties it, with @p pages pages (at most `MaxPages`). */
                void reset(uint32_t pages);

                /**
                 * @brief Takes @p count free pages in a row: the lowest such range, or with
                 *        @p fromTop the highest (so ranges kept for good gather at the top, away
                 *        from those that come and go).
                 * @return The range's first page, or `None`.
                 */
                uint32_t take(uint32_t count, bool fromTop = false);

                /**
                 * @brief Takes the @p count pages from @p first if all of them are free (a range
                 *        grown in place).
                 * @return True if taken.
                 */
                bool takeAt(uint32_t first, uint32_t count);

                /** @brief Frees the @p count pages from @p first (taken before). */
                void give(uint32_t first, uint32_t count);

                /** @brief True if page @p page is in use. */
                bool isUsed(uint32_t page) const { return (_used[page / 32] & (1u << (page % 32))) != 0; }

                /** @brief Returns its pages. */
                uint32_t pages() const { return _pages; }

                /** @brief Returns the pages in use. */
                uint32_t usedPages() const { return _usedPages; }

                /** @brief Returns the free pages. */
                uint32_t freePages() const { return _pages - _usedPages; }

                /** @brief Returns the longest range of free pages. */
                uint32_t longestFree() const { return _longest; }

        private:
                void     mark(uint32_t first, uint32_t count, bool used);
                uint32_t findLongest() const;

                uint32_t _used[MaxPages / 32] = {};
                uint32_t _pages = 0;
                uint32_t _usedPages = 0;
                uint32_t _longest = 0;
};

/**
 * @brief Gathering physical pages from a heap in runs (`devplan/psramwindow.md` §3b): a run is a
 *        block of whole pages aligned to a page, the largest the heap has first, until there
 *        are enough. Portable: the heap is a `Source`.
 */
namespace PageRuns {

        /** @brief A run: its pages from @p at on (an address the heap gave). */
        struct Run {
                        uint8_t *at = nullptr;
                        uint32_t pages = 0;
        };

        /** @brief The most runs one gathering keeps: past that, the heap is too cut up. */
        constexpr size_t MaxRuns = 16;

        /** @brief Where runs come from. */
        class Source {
                public:
                        /** @brief Returns a lower bound on the largest free block, in bytes. */
                        virtual size_t largestFree() = 0;

                        /**
                         * @brief Allocates @p pages pages, aligned to a page, and a cache line
                         *        after them for the heap's own writes.
                         * @return The run's start, or null.
                         */
                        virtual uint8_t *allocRun(uint32_t pages) = 0;

                        /** @brief Frees a run from `allocRun()`. */
                        virtual void freeRun(uint8_t *at) = 0;

                protected:
                        ~Source() = default;
        };

        /**
         * @brief Gathers @p pages pages into @p out: each time the largest run the source's
         *        largest block holds (aligned, its line after it), one smaller if that fails,
         *        then halved.
         * @return How many runs (all of @p pages), or 0 with nothing kept (the source can't give
         *         that many in `out.size()` runs).
         */
        size_t gather(Source &source, uint32_t pages, uint32_t pageBytes, Span<Run> out);

        /** @brief Frees @p runs back to @p source. */
        void release(Source &source, Span<const Run> runs);

} // namespace PageRuns

BMDS_NAMESPACE_END
