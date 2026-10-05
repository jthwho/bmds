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
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A write-through cache of 512-byte sectors, shared by the FatFs
 *        volumes (devplan/storage.md §3).
 *
 * FatFs keeps one sector of metadata (its window), so every lookup in a
 * directory reads the directory from the card again. The port keeps what it
 * reads into the window here: directories, the FAT, exFAT's bitmap. Writes
 * go to the device as before and update the copies they cover, whatever
 * they carry; a failed write or a trim drops them. Least recently used
 * sectors make room.
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

                /** @brief The sectors it holds (32 KB of Psram, plus 1 KB of tags). */
                static constexpr size_t Sectors = 64;

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

                /** @brief Keeps a copy of sector @p sector of @p drive, just read from the device. */
                void store(uint8_t drive, uint64_t sector, const uint8_t *data);

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
                static constexpr uint8_t NoDrive = 0xff;

                struct Slot {
                                uint64_t sector;
                                uint32_t used;  // _clock at the last use
                                uint8_t  drive; // NoDrive if empty
                };

                Slot    *find(uint8_t drive, uint64_t sector);
                uint8_t *dataOf(const Slot &slot) { return _data + (&slot - _slots) * SectorBytes; }

                Slot    *_slots = nullptr; // Sectors of them, then the data
                uint8_t *_data = nullptr;
                uint32_t _clock = 0;
};

BMDS_NAMESPACE_END
