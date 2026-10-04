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
 * @brief A block device in memory.
 *
 * Stands in for a disk in the host tests, and can serve as a RAM disk. The
 * sectors are one allocation from the `MemSpace` given to `allocate()`,
 * zero-filled; `trim()` leaves them as they are. `setPresent(false)`
 * simulates a removed medium.
 *
 * @par Thread Safety
 * Thread-safe, except `allocate()`, which must happen before the device is
 * shared.
 */
class RamBlockDevice final : public BlockDevice {
        public:
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
                 * @brief Allocates @p sectorCount zeroed sectors, replacing any there were.
                 * @return `Ok`, `NoMem`, or `Invalid` (no sectors, or too many to address).
                 */
                Error allocate(uint64_t sectorCount, MemSpace space);

                /** @brief Simulates inserting (true) or removing (false) the medium. */
                void setPresent(bool present);

                /** @brief Returns the sectors, for inspection in tests. */
                ByteSpan contents() const { return ByteSpan(_data, _bytes); }

                uint32_t sectorSize() const override { return _sectorSize; }
                uint64_t sectorCount() const override;
                bool     isPresent() const override;
                Error    sync() override;
                Error    trim(uint64_t first, uint64_t count) override;

        protected:
                Error readSectors(uint64_t sector, WritableByteSpan buf) override;
                Error writeSectors(uint64_t sector, ByteSpan data) override;

        private:
                mutable Mutex  _lock;
                uint8_t       *_data = nullptr;
                size_t         _bytes = 0;
                const uint32_t _sectorSize;
                bool           _present = true;
};

BMDS_NAMESPACE_END
