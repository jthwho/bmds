/**
 * @file      sectorcache.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the cache of filesystem metadata under FatFs.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A write-through cache of 512-byte sectors, shared by the FatFs
 *        volumes (devplan/storage.md §3).
 *
 * FatFs keeps one sector of metadata (its window), so every lookup in a
 * directory reads the directory again. The port keeps what it reads into
 * the window here: directories, the FAT, exFAT's bitmap. A miss reads a run
 * of `ReadAhead` sectors in one request (`fill()`), since directories and the
 * FAT lie in runs. Writes go to the device as before and update the copies
 * they cover, whatever they carry; a failed write or a trim drops them.
 *
 * Sectors are found by a hash of drive and sector. Room is made by a
 * segmented LRU: a sector enters on probation and a hit protects it (at most
 * three quarters of the cache); probation's oldest goes first. A sector read
 * ahead is protected by its second hit: the scan that read it ahead uses it
 * once. A directory
 * bigger than the cache, scanned again and again, then churns probation and
 * leaves the rest alone.
 *
 * Writes that bypass FatFs (a raw write to a mounted volume's device) are
 * not seen, as FatFs's own window doesn't see them.
 *
 * @par Thread Safety
 * Not thread-safe: the storage lock (`Fat::lock()`) covers it.
 */
class SectorCache {
        public:
                /** @brief The sector size it caches. */
                static constexpr size_t SectorBytes = 512;

                /** @brief The sectors it holds (128 KB of Psram, plus 4.5 KB of tags). */
                static constexpr size_t Sectors = 256;

                /** @brief The sectors a miss reads in one request (4 KB). */
                static constexpr size_t ReadAhead = 8;

                constexpr SectorCache() = default;
                SectorCache(const SectorCache &) = delete;
                SectorCache &operator=(const SectorCache &) = delete;

                /**
                 * @brief Allocates the cache (Psram); idempotent.
                 * @return `Ok`, or `NoMem` (the cache then stays off: every
                 *         lookup misses).
                 */
                Error init();

                /**
                 * @brief Copies sector @p sector of drive @p drive into @p out
                 *        if it is cached.
                 * @return true on a hit.
                 */
                bool read(uint8_t drive, uint64_t sector, uint8_t *out);

                /**
                 * @brief Reads sector @p sector of drive @p drive (@p device) into
                 *        @p out after a miss, with the sectors after it, keeping
                 *        them all.
                 * @return `Ok`, or the device's error.
                 */
                Error fill(uint8_t drive, BlockDevice &device, uint64_t sector, uint8_t *out);

                /**
                 * @brief Updates the cached sectors among the @p count written
                 *        from @p data at @p sector.
                 */
                void written(uint8_t drive, uint64_t sector, size_t count, const uint8_t *data);

                /** @brief Drops the cached sectors of @p drive in [@p sector, @p sector + @p count). */
                void discard(uint8_t drive, uint64_t sector, uint64_t count);

                /** @brief Drops every cached sector of @p drive (a mount, an unmount, a format). */
                void discard(uint8_t drive);

        private:
                static constexpr uint16_t None = 0xffff;
                static constexpr size_t   ProtectedMax = Sectors * 3 / 4;

                static_assert((Sectors & (Sectors - 1)) == 0 && Sectors < None, "a power of two, indexed by uint16_t");

                // The lists a slot is on: most recent at the head.
                enum List : uint8_t {
                        Free,
                        Probation,
                        Protected,
                        Lists
                };

                struct Slot {
                                uint64_t sector;
                                uint16_t prev;     // in its list
                                uint16_t next;     // in its list
                                uint16_t hashNext; // in its bucket
                                uint8_t  drive;
                                uint8_t  list : 2;  // a List
                                uint8_t  ahead : 1; // read ahead, not asked for yet: its first hit isn't a reuse
                };

                static_assert(sizeof(Slot) == 16, "the tags' size (4.5 KB with the buckets)");

                struct Ring {
                                uint16_t head;
                                uint16_t tail;
                                uint16_t count;
                };

                static size_t bucketOf(uint8_t drive, uint64_t sector);
                uint16_t      find(uint8_t drive, uint64_t sector) const;
                void          unlink(uint16_t i);
                void          pushHead(List list, uint16_t i);
                void          hashRemove(uint16_t i);
                void          release(uint16_t i); // emptied, onto the free list
                uint16_t      take();              // a slot to fill: free, else probation's oldest, else protected's
                void          keep(uint8_t drive, uint64_t sector, const uint8_t *data, bool ahead); // on probation
                uint8_t      *dataOf(uint16_t i) { return _data + size_t(i) * SectorBytes; }

                Slot     *_slots = nullptr;   // Sectors of them
                uint16_t *_buckets = nullptr; // Sectors of them, chains through Slot::hashNext
                uint8_t  *_data = nullptr;    // Sectors × SectorBytes
                uint8_t  *_ahead = nullptr;   // a run read on a miss (PsramDma: read without a bounce)
                Ring      _rings[Lists] = {};
};

BMDS_NAMESPACE_END
