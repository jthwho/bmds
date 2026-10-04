/**
 * @file      blockdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A disk: an array of fixed-size sectors.
 *
 * The filesystem's view of storage. `SdCard` implements it on the target,
 * `RamBlockDevice` in memory (the host tests, a RAM disk).
 *
 * `read()` and `write()` move whole sectors and check their arguments
 * before calling the implementation: a buffer that is empty or not a
 * multiple of `sectorSize()` is `Invalid`, a range past `sectorCount()` is
 * `OutOfRange`, and a device that `isPresent()` says is gone is `NoMedia`.
 * Implementations only see valid requests (though a removable device can
 * still lose its medium during one, and then reports `NoMedia` itself).
 *
 * The owner of a device destroys it as its own type, never through a
 * `BlockDevice` pointer.
 *
 * @par Thread Safety
 * Implementations are thread-safe: requests from different tasks are
 * serialized by the device.
 */
class BlockDevice {
        public:
                /** @brief Returns the sector size in bytes (512 for SD cards). */
                virtual uint32_t sectorSize() const = 0;

                /** @brief Returns the number of sectors (0 while there is no medium). */
                virtual uint64_t sectorCount() const = 0;

                /**
                 * @brief Returns true while the medium is there.
                 *
                 * Always true for fixed devices. A snapshot: a removable
                 * medium can go at any moment.
                 */
                virtual bool isPresent() const { return true; }

                /**
                 * @brief Returns the erase block (allocation unit) in sectors, 1 if unknown.
                 *
                 * Formatting aligns the filesystem's data area to it.
                 */
                virtual uint32_t eraseBlockSectors() const { return 1; }

                /**
                 * @brief Reads whole sectors.
                 * @param sector First sector.
                 * @param buf    Destination; its size is a multiple of `sectorSize()`.
                 * @return `Ok`, `Invalid`, `OutOfRange`, `NoMedia`, or a device error
                 *         (`IoError`, `Timeout`, `CorruptData`, ...).
                 * @blocking
                 */
                Error read(uint64_t sector, WritableByteSpan buf);

                /**
                 * @brief Writes whole sectors.
                 * @param sector First sector.
                 * @param data   Source; its size is a multiple of `sectorSize()`.
                 * @return As for `read()`, and `ReadOnly`.
                 * @blocking
                 */
                Error write(uint64_t sector, ByteSpan data);

                /**
                 * @brief Waits until written data is on the medium.
                 * @return `Ok`, `NoMedia`, or a device error.
                 * @blocking
                 */
                virtual Error sync() = 0;

                /**
                 * @brief Tells the device a range of sectors is no longer in use.
                 *
                 * Advisory (the contents of trimmed sectors are undefined
                 * afterwards); a device without the notion returns
                 * `NotSupported`, which callers may ignore.
                 * @param first First sector.
                 * @param count Number of sectors.
                 * @blocking
                 */
                virtual Error trim(uint64_t first, uint64_t count);

        protected:
                constexpr BlockDevice() = default;

                // Not virtual: block devices are never deleted through a
                // BlockDevice pointer (and so no operator delete is linked).
                ~BlockDevice() = default;

                /** @brief Reads a valid range of sectors (`buf` is non-empty and whole sectors). */
                virtual Error readSectors(uint64_t sector, WritableByteSpan buf) = 0;

                /** @brief Writes a valid range of sectors (`data` is non-empty and whole sectors). */
                virtual Error writeSectors(uint64_t sector, ByteSpan data) = 0;

                /** @brief Checks a request of @p bytes at @p sector, as `read()` and `write()` do. */
                Error checkRequest(uint64_t sector, size_t bytes) const;
};

BMDS_NAMESPACE_END
