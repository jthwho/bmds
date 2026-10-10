/**
 * @file      blockdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A disk: an array of fixed-size sectors.
 *
 * The filesystem's view of storage. `SdCard` implements it on the target,
 * `RamBlockDevice` in memory (the host tests, a RAM disk), `MemoryBlockDevice`
 * over memory read in place (a ROM image).
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
class FlashDevice;

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
                 * @brief Returns true if the sectors are in memory, so a read costs a
                 *        copy: a cache in front of it would only take room
                 *        (`devplan/storage.md` §3). False by default.
                 */
                virtual bool isMemory() const { return false; }

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

                /** @brief What it has done (`devplan/sysmon.md` §3.2), since it was made. */
                struct IoStats {
                                uint32_t reads = 0;        ///< Read requests.
                                uint32_t writes = 0;       ///< Write requests.
                                uint64_t readBytes = 0;    ///< Bytes read.
                                uint64_t writtenBytes = 0; ///< Bytes written.
                                uint32_t errors = 0;       ///< Requests the device failed (a medium gone not counted).
                                uint64_t ioUs = 0;   ///< Time in requests, in all (overlapping ones each counted).
                                uint32_t erases = 0; ///< Blocks erased (a flash).
                };

                /** @brief Returns what it has done. @nonblocking */
                IoStats ioStats() const;

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

                /**
                 * @brief Returns the device as a NOR flash, if it is one (littlefs is
                 *        then mounted on it instead of FAT: devplan/storage.md §13d);
                 *        else null.
                 */
                virtual FlashDevice *flash() { return nullptr; }

                /**
                 * @brief Returns the device's bytes where they can be read in place (a
                 *        flash mapped through the cache, an image in memory), valid while
                 *        the device is; empty if they can't be. A ROM image there is
                 *        mounted as one, used in place (`devplan/rom.md` §3d).
                 */
                virtual ByteSpan mapped() const { return ByteSpan(); }

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

                /**
                 * @brief Counts a request made another way than `read()` and `write()`
                 *        (a flash's own calls): started at @p startedUs (monotonic).
                 */
                void countRequest(bool write, size_t bytes, Error err, int64_t startedUs);

                /** @brief Counts a block erased. */
                void countErase() { _erases.fetch_add(1, std::memory_order_relaxed); }

        private:
                std::atomic<uint32_t> _reads{0};
                std::atomic<uint32_t> _writes{0};
                std::atomic<uint32_t> _errors{0};
                std::atomic<uint64_t> _readBytes{0};
                std::atomic<uint64_t> _writtenBytes{0};
                std::atomic<uint64_t> _ioUs{0};
                std::atomic<uint32_t> _erases{0};
};

BMDS_NAMESPACE_END
