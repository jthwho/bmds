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
 * Metadata writes may be held instead (`hold()`), at most `MaxHeld`, and
 * written back by `flush()` in an order a cut can't turn into a directory
 * entry pointing at free clusters (devplan/storage.md §3): allocation
 * sectors with every cluster used before or now marked used, then the
 * directories, then the allocation sectors as they are, then the rest.
 * Reads find the held copy; a direct write over one supersedes it.
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

                /** @brief The most sectors held for writing back (32 KB, and as much for originals). */
                static constexpr size_t MaxHeld = 64;

                /** @brief What a held sector is: how it is written back. */
                enum class Kind : uint8_t {
                        Directory, ///< Written in the middle phase.
                        Fat16,     ///< FAT entries of 16 bits (allocation).
                        Fat32,     ///< FAT entries of 32 bits, 28 used (allocation; exFAT's too).
                        Bitmap,    ///< exFAT's allocation bitmap.
                        Other,     ///< FSInfo, boot sectors: written last.
                };

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

                /**
                 * @brief Holds @p data as sector @p sector of @p drive (@p device), of
                 *        @p kind, to write back later (`flush()`).
                 * @return `Ok`, `Full` (`MaxHeld` held: flush, then hold again),
                 *         `NoMem` (no cache), or the device's error reading an
                 *         allocation sector's original.
                 */
                Error hold(uint8_t drive, BlockDevice &device, uint64_t sector, const uint8_t *data, Kind kind);

                /** @brief Returns true if @p drive has sectors held. */
                bool hasHeld(uint8_t drive) const;

                /** @brief Returns true if a held allocation sector frees something (`isFreeHeld()` may say yes). */
                bool hasFreesHeld() const { return _freesHeld > 0; }

                /**
                 * @brief Returns true if entry @p entry of allocation sector @p sector
                 *        (a bit of a bitmap sector, an entry of a FAT sector) was used on
                 *        the card and freed since (used again or not): its cluster mustn't
                 *        be written until the free is (`devplan/storage.md` §3).
                 */
                bool isFreeHeld(uint8_t drive, uint64_t sector, size_t entry) const;

                /**
                 * @brief Writes @p drive's held sectors to @p device in the safe order,
                 *        coalescing neighbours, and keeps them as clean copies.
                 * @return `Ok`, or the first write's error (what was held is dropped).
                 */
                Error flush(uint8_t drive, BlockDevice &device);

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
                        Held, // never taken to make room
                        Lists
                };

                struct Slot {
                                uint64_t sector;
                                uint16_t prev;     // in its list
                                uint16_t next;     // in its list
                                uint16_t hashNext; // in its bucket
                                uint8_t  drive;
                                uint8_t  list : 3;  // a List
                                uint8_t  ahead : 1; // read ahead, not asked for yet: its first hit isn't a reuse
                                uint8_t  kind : 3;  // a Kind, while held
                                uint8_t  frees : 1; // held, and it frees entries used on the card
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
                int            heldIndex(uint16_t i) const; // in _held, or -1
                static bool    entryUsed(Kind kind, const uint8_t *sector, size_t entry);
                bool           noteFrees(size_t at); // _held[at]'s entries freed since its original, kept: any?
                void           unhold(size_t at);    // _held[at] clean again, on probation
                Error          writeRun(uint8_t drive, BlockDevice &device, const uint16_t *order, size_t count,
                                        int phase);             // one phase's sectors, in runs
                const uint8_t *phaseData(size_t at, int phase); // what _held[at] is written as in @p phase

                Slot     *_slots = nullptr;   // Sectors of them
                uint16_t *_buckets = nullptr; // Sectors of them, chains through Slot::hashNext
                uint8_t  *_data = nullptr;    // Sectors × SectorBytes
                uint8_t  *_ahead = nullptr;   // a run read on a miss, or written back (PsramDma: no bounce)
                uint8_t  *_originals = nullptr; // MaxHeld × SectorBytes: an allocation sector before its change
                uint8_t  *_freed = nullptr;     // MaxHeld × SectorBytes: a bit per entry freed since (sticky)
                uint8_t  *_merged = nullptr;    // a sector as phase 1 writes it
                uint16_t *_held = nullptr;      // MaxHeld: the slots held
                size_t    _heldCount = 0;
                size_t    _freesHeld = 0; // held slots with frees
                Ring      _rings[Lists] = {};
};

BMDS_NAMESPACE_END
