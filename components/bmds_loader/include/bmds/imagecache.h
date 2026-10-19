/**
 * @file      imagecache.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/programimage.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Programs read from their files, kept for the next `exec`: each
 *        one's checked, unrelocated bytes and its patches
 *        (`ProgramImage::read()`), which `ProgramImage::place()` turns into an
 *        image at any address without the file (devplan/programs.md §14.1).
 *
 * An entry is found by its file's `Key`. Entries are kept within a budget,
 * least recently used ones going first, and are dropped when their file
 * may change (`changed()`) or memory runs short (`reclaim()`: the cache is
 * the `Memory::Reclaimer`). An entry is pinned while it is in use; one
 * dropped meanwhile is freed by its last `release()`.
 *
 * Nothing done under the cache's lock allocates or calls out, so
 * `changed()` may be called with the storage lock held and `reclaim()` from
 * a failed allocation.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
class ImageCache final : public Memory::Reclaimer {
        public:
                /** @brief A program file, as `Vfs::stat()` describes it (links followed). */
                struct Key {
                                uint32_t device = 0;   ///< `FileInfo::device` (0: not cacheable).
                                uint64_t id = 0;       ///< `FileInfo::id` (0: not cacheable).
                                uint64_t size = 0;     ///< Its size.
                                int64_t  modified = 0; ///< Its modification time (ns).

                                /** @brief Returns true if a file with this key can be cached. */
                                bool isCacheable() const { return device != 0 && id != 0; }

                                bool operator==(const Key &other) const = default;
                };

                /** @brief One program: what `ProgramImage::place()` needs. */
                class Entry {
                        public:
                                /** @brief What `ProgramImage::inspect()` said. */
                                const ProgramImage::Info &info() const { return _info; }

                                /** @brief The patches (`info().relocations`). */
                                ProgramImage::Patch *patches() {
                                        return reinterpret_cast<ProgramImage::Patch *>(this + 1);
                                }

                                /**
                                 * @brief The file bytes (`info().fileSize`); with `hasPages()`, page-aligned:
                                 *        page `p` of the program is `bytes() + p × SharePageBytes`.
                                 */
                                uint8_t *bytes() {
                                        return _pages != nullptr
                                                       ? _pages
                                                       : reinterpret_cast<uint8_t *>(patches() + _info.relocations);
                                }

                                /**
                                 * @brief True if the bytes are page-aligned, so the program's shared pages
                                 *        can be mapped where they are (devplan/programs.md §14.2).
                                 */
                                bool hasPages() const { return _pages != nullptr; }

                        private:
                                friend class ImageCache;
                                Entry             *_prev = nullptr;
                                Entry             *_next = nullptr;
                                Key                _key;
                                ProgramImage::Info _info;
                                size_t             _bytes = 0;       // the whole allocation (and the pages')
                                uint8_t           *_pages = nullptr; // the bytes, page-aligned apart, if any
                                uint32_t           _generation = 0; // the cache's when its file was looked at
                                uint32_t           _pins = 0;
                                bool               _listed = false; // in the cache (else freed when unpinned)
                };


                /**
                 * @brief A cache keeping up to @p budgetBytes (0: none kept). With
                 *        @p pageAlign, an entry for a program with shared pages has its bytes
                 *        aligned to it (`Entry::hasPages()`).
                 */
                explicit ImageCache(size_t budgetBytes, size_t pageAlign = 0)
                    : _budget(budgetBytes), _pageAlign(pageAlign) {}

                ~ImageCache();
                ImageCache(const ImageCache &) = delete;
                ImageCache &operator=(const ImageCache &) = delete;

                /**
                 * @brief The current generation: read it before looking at a file
                 *        (`Vfs::stat()`), and give it to `create()`.
                 */
                uint32_t generation() const;

                /**
                 * @brief Finds the entry for @p key and pins it.
                 * @return The entry (give it back with `release()`), or null.
                 */
                Entry *acquire(const Key &key);

                /**
                 * @brief A new entry for @p key's program, pinned, not yet kept: fill
                 *        its bytes and patches (`ProgramImage::read()`), then `keep()` or
                 *        `release()` it.
                 * @param generation `generation()` from before @p key was looked at.
                 * @return The entry, or null (no memory).
                 */
                Entry *create(const Key &key, const ProgramImage::Info &info, uint32_t generation);

                /**
                 * @brief Keeps @p entry (from `create()`) for later lookups, if it may:
                 *        its file unchanged since its generation, no entry for its key
                 *        yet, and room in the budget (the least recently used unpinned
                 *        entries go). Unpins it either way.
                 * @return True if it was kept.
                 */
                bool keep(Entry *entry);

                /** @brief Unpins @p entry; one not kept (or dropped meanwhile) is freed. */
                void release(Entry *entry);

                /**
                 * @brief Pins @p entry once more (an instance mapping its pages, for as long
                 *        as it runs): give it back with `release()`.
                 */
                void pin(Entry *entry);

                /**
                 * @brief The file @p id on @p device may change: its entries go, and
                 *        the generation moves on. `(0, 0)`: every file.
                 */
                void changed(uint32_t device, uint64_t id);

                /** @brief Drops every unpinned entry. @return The bytes freed. */
                size_t clear();

                /** @copydoc Memory::Reclaimer::reclaim */
                size_t reclaim(MemSpace space, size_t bytes) override;

                /** @copydoc Memory::Reclaimer::usage */
                Usage usage() const override;

        private:
                // Lock held for all of these.
                void pushFront(Entry *entry); // lists it as the most recently used
                void unlink(Entry *entry);
                void drop(Entry *entry); // unlinks; frees it unless pinned
                bool evictOne();         // the least recently used unpinned entry; false if none
                static void destroy(Entry *entry); // frees it (and its pages)

                mutable Mutex _lock;
                const size_t  _budget;
                const size_t  _pageAlign;
                Entry        *_first = nullptr; // most recently used first
                Entry        *_last = nullptr;
                size_t        _held = 0; // bytes of the listed entries
                size_t        _count = 0;
                uint32_t      _generation = 0;
                uint32_t      _hits = 0;
                uint32_t      _misses = 0;
                uint32_t      _reclaims = 0;
};

BMDS_NAMESPACE_END
