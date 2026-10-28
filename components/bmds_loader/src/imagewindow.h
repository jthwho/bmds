/**
 * @file      imagewindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' instances sharing the pages of their image through the MMU (devplan/programs.md
 * §14.2).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/imagecache.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A block of PSRAM addresses, reserved at boot, where an instance of a program is
 *        mapped page by page: its shared pages straight from its cache entry's bytes, its
 *        private pages from memory of its own.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
namespace ImageWindow {

        /** @brief An instance mapped into the window. */
        struct Mapping {
                        uint8_t           *base = nullptr;         ///< Where the image runs (the range's start).
                        uint32_t           pages = 0;              ///< The range's pages.
                        uint32_t           shared = 0;             ///< Of them, mapped from the entry.
                        uint8_t          **privatePages =
                                nullptr; // `pages` slots: each private page in the range; null for a shared one
                        uint8_t           *privateBlock = nullptr; // the private pages, one after another
                        ImageCache::Entry *entry = nullptr;        // pinned while mapped
                        ImageCache        *cache = nullptr;

                        /** @brief True if it holds a mapping. */
                        bool isMapped() const { return base != nullptr; }
        };

        /**
         * @brief Reserves @p bytes of the PSRAM window's addresses, once, for good (0: the
         *        window is off and every program is a plain copy).
         * @return `Ok`, or the error reserving them (the window then stays off).
         */
        Error init(size_t bytes);

        /** @brief True if the window can map programs. */
        bool isEnabled();

        /** @brief True if @p bytes at @p p lie in the window (for DMA checks). @isrsafe */
        bool contains(const void *p, size_t bytes);

        /**
         * @brief Maps an instance of @p entry's program (`entry.hasPages()`): a range, its
         *        private pages placed for it (`ProgramImage::placePages()`) and written back,
         *        every page mapped, the range invalidated. @p entry is pinned (`cache`) until
         *        `unmap()`.
         * @return `Ok`, or `NoMem` (no range, or no memory for its private pages: the caller
         *         makes a plain copy instead).
         * @blocking Allocates, copies the private pages.
         */
        Error map(ImageCache &cache, ImageCache::Entry &entry, Mapping &out);

        /**
         * @brief Ends a mapping: the range invalidated (its dirty lines dropped), unmapped and
         *        freed with the private pages, the entry unpinned. No-op if not mapped.
         * @blocking
         */
        void unmap(Mapping &mapping);

        /** @brief What `mem` shows. */
        struct Usage {
                        uint32_t pages = 0;     ///< The window's pages.
                        uint32_t used = 0;      ///< In use by instances.
                        uint32_t instances = 0; ///< Mapped now.
                        uint32_t shared = 0;    ///< Pages mapped from entries (each saving a page).
                        uint32_t mapped = 0;    ///< Instances mapped since boot.
                        uint32_t fallbacks = 0; ///< Of programs with shared pages, plain copies instead (no room).
        };

        /** @brief Returns the counts. */
        Usage usage();

        /** @brief Counts a program with shared pages that became a plain copy. */
        void countFallback();

        /**
         * @brief Returns the physical page behind PSRAM address @p p (a heap address, or a
         *        mapped window one), read from the MMU's table under its lock.
         * @isrsafe
         */
        uint32_t physicalPage(const void *p);

        /**
         * @brief Points the PSRAM page at @p vaddr (an address outside the heap's range that the
         *        caller reserved) to physical page @p page, under the MMU's lock. The caller keeps
         *        the caches right (the file's notes). For `memory.window` (`devplan/psramwindow.md`
         *        step 1) until `PsramWindow` takes these over.
         */
        void writeEntry(void *vaddr, uint32_t page);

        /**
         * @brief `writeEntry()` for @p count pages from @p vaddr, to physical pages from
         *        @p firstPage on, under one hold of the MMU's lock.
         */
        void writeEntries(void *vaddr, uint32_t firstPage, uint32_t count);

        /** @brief Makes the PSRAM page at @p vaddr invalid, under the MMU's lock. */
        void clearEntry(void *vaddr);

} // namespace ImageWindow

BMDS_NAMESPACE_END
