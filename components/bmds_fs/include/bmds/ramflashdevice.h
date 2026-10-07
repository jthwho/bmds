/**
 * @file      ramflashdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/flashdevice.h>
#include <bmds/flashqueue.h>
#include <bmds/memspace.h>
#include <bmds/mutex.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A NOR flash in memory, for the host tests: littlefs mounts on it
 *        (devplan/storage.md §13d).
 *
 * An erase sets a block to 0xFF and a program clears bits, as on the
 * flash. With `setQueued()`, writes wait in a `FlashQueue` until `drain()`
 * does them, as `FlashPartition`'s worker would, so reads go through the
 * queue's overlay.
 *
 * As a block device it reads in 512-byte sectors and refuses writes
 * (`ReadOnly`): a raw write would break the filesystem on it.
 *
 * @par Thread Safety
 * Thread-safe (one lock).
 */
class RamFlashDevice final : public BlockDevice, public FlashDevice {
        public:
                /** @brief The sector size it shows as a block device. */
                static constexpr uint32_t SectorBytes = 512;

                RamFlashDevice() = default;
                ~RamFlashDevice();
                RamFlashDevice(const RamFlashDevice &) = delete;
                RamFlashDevice &operator=(const RamFlashDevice &) = delete;

                /**
                 * @brief Allocates @p blocks erased blocks of @p blockBytes (a multiple of 512) in @p space.
                 * @return `Ok`, `Invalid`, `InvalidState` (done before), or `NoMem`.
                 */
                Error allocate(uint32_t blockBytes, uint32_t blocks, MemSpace space);

                /**
                 * @brief Queues writes from now on, in a ring of @p bytes, until `drain()`.
                 * @return `Ok`, or as for `FlashQueue::init()`.
                 */
                Error setQueued(size_t bytes);

                /** @brief Does up to @p count queued operations; returns how many it did. */
                size_t drain(size_t count);

                /** @brief Returns how many operations are queued. */
                size_t queued() const;

                /**
                 * @brief Copies the flash as it is into @p out (`blockBytes() * blockCount()`
                 *        bytes), without the queue: what a power cut would leave.
                 * @return `Ok` or `Invalid` (the size).
                 */
                Error snapshot(WritableByteSpan out) const;

                /**
                 * @brief Replaces the flash's contents with @p image (as `snapshot()` gave
                 *        them); anything queued is dropped.
                 * @return `Ok` or `Invalid` (the size).
                 */
                Error load(ByteSpan image);

                // BlockDevice
                uint32_t     sectorSize() const override { return SectorBytes; }
                uint64_t     sectorCount() const override { return uint64_t(_blockBytes) * _blocks / SectorBytes; }
                Error        sync() override { return Error::Ok; }
                FlashDevice *flash() override { return this; }

                // FlashDevice
                uint32_t blockBytes() const override { return _blockBytes; }
                uint32_t blockCount() const override { return _blocks; }
                Error    read(uint32_t address, WritableByteSpan buf) override;
                Error    program(uint32_t address, ByteSpan data) override;
                Error    erase(uint32_t block) override;
                Error    flush(unsigned int timeoutMs) override;

        protected:
                Error readSectors(uint64_t sector, WritableByteSpan buf) override;
                Error writeSectors(uint64_t, ByteSpan) override { return Error::ReadOnly; }

        private:
                bool  fits(uint64_t address, size_t bytes) const;
                void  apply(const FlashQueue::Op &op); // on the memory; lock held
                Error drainOneLocked();

                mutable Mutex _lock;
                uint8_t      *_bytes = nullptr;
                uint32_t      _blockBytes = 0;
                uint32_t      _blocks = 0;
                FlashQueue    _queue;
                bool          _queued = false;
};

BMDS_NAMESPACE_END
