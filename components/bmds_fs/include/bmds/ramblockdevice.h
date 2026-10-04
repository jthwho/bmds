/**
 * @file      ramblockdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/memspace.h>
#include <bmds/mutex.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A block device in memory, sparse.
 *
 * Stands in for a disk in the host tests and serves as the RAM disk. The
 * sectors are kept in chunks of `ChunkBytes` (whole sectors), allocated from
 * the `MemSpace` given to `allocate()` when first written with anything but
 * zeros, and freed when `trim()` leaves one all zeros (trimmed sectors
 * become zeros; FatFs trims the clusters of deleted files), so the
 * device's size is only a limit: a
 * fresh filesystem on it costs a chunk or two. Unwritten sectors read as
 * zeros. A write that needs a chunk when memory has run out fails with
 * `NoMem`. `setPresent(false)` simulates a removed medium.
 *
 * @par Thread Safety
 * Thread-safe, except `allocate()`, which must happen before the device is
 * shared.
 */
class RamBlockDevice final : public BlockDevice {
        public:
                /** @brief Bytes per chunk (rounded down to whole sectors, at least one). */
                static constexpr size_t ChunkBytes = 32 * 1024;

                /**
                 * @brief Constructs an empty device (no sectors until `allocate()`).
                 * @param sectorSize Sector size in bytes (non-zero).
                 */
                explicit RamBlockDevice(uint32_t sectorSize);

                /** @brief Frees the sectors. */
                ~RamBlockDevice();

                RamBlockDevice(const RamBlockDevice &) = delete;
                RamBlockDevice &operator=(const RamBlockDevice &) = delete;

                /**
                 * @brief Sets the size to @p sectorCount zeroed sectors, dropping any
                 *        contents; chunks come from @p space as they are written.
                 *
                 * Allocates only the chunk table (a pointer per chunk).
                 *
                 * @return `Ok`, `NoMem`, or `Invalid` (no sectors, or too many to address).
                 */
                Error allocate(uint64_t sectorCount, MemSpace space);

                /** @brief Simulates inserting (true) or removing (false) the medium. */
                void setPresent(bool present);

                /** @brief Returns the memory the written chunks take, in bytes. */
                size_t usedBytes() const;

                uint32_t sectorSize() const override { return _sectorSize; }
                uint64_t sectorCount() const override;
                bool     isPresent() const override;
                Error    sync() override;
                Error    trim(uint64_t first, uint64_t count) override;

        protected:
                Error readSectors(uint64_t sector, WritableByteSpan buf) override;
                Error writeSectors(uint64_t sector, ByteSpan data) override;

        private:
                void freeChunks();

                mutable Mutex  _lock;
                uint8_t      **_chunks = nullptr; // one per chunk; null: all zeros
                size_t         _chunkCount = 0;
                size_t         _usedChunks = 0;
                uint64_t       _sectors = 0;
                const uint32_t _sectorSize;
                const uint32_t _chunkSectors; // sectors per chunk
                MemSpace       _space = MemSpace::Psram;
                bool           _present = true;
};

BMDS_NAMESPACE_END
