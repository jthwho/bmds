/**
 * @file      flashpartition.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <esp_partition.h>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/flashdevice.h>
#include <bmds/flashqueue.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A flash partition as a `FlashDevice`, with an ordered write cache
 *        (devplan/storage.md §13d): littlefs is mounted on it.
 *
 * Erases and programs go into a `FlashQueue` (Psram) and return; the worker
 * task (`flash0`, internal stack) does them in order, one at a time, once
 * writing has paused for `DelayMs` (at most `MaxDelayMs` after it began, at
 * once for `flush()` or a queue half full; while `FlashWrites` is held, not
 * until it goes, devplan/cfg.md §3). A flash operation stops both
 * cores, so done during a close it would hold that up too: waiting lets the
 * close return first, as FAT's cache did (§13a). Reads
 * copy from the partition mapped into the address space and lay the queue
 * over it, so they see every write at once. Only the worker touches the
 * flash, so every call works from any task on any stack.
 *
 * While the flash erases or programs, the caches are off and the other core
 * waits (the IDF's rule), so a read never sees half an operation. A write
 * that finds the queue full waits for the worker to make room.
 *
 * As a block device it reads in 512-byte sectors and refuses writes
 * (`ReadOnly`): a raw write would break littlefs.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class FlashPartition final : public BlockDevice, public FlashDevice {
        public:
                /** @brief The queue's ring. */
                static constexpr size_t QueueBytes = 64 * 1024;

                /** @brief Quiet time before the queue is written. */
                static constexpr unsigned DelayMs = 100;

                /** @brief The longest an operation waits while writing goes on. */
                static constexpr unsigned MaxDelayMs = 1000;

                /** @brief A flash sector: the erase block. */
                static constexpr uint32_t BlockBytes = 4096;

                /** @brief The sector size it shows as a block device. */
                static constexpr uint32_t SectorBytes = 512;

                FlashPartition() = default;
                FlashPartition(const FlashPartition &) = delete;
                FlashPartition &operator=(const FlashPartition &) = delete;

                /**
                 * @brief Maps @p partition and starts the worker.
                 * @return `Ok`, `InvalidState` (done before), `Invalid` (not whole
                 *         blocks), `NoMem`, or `IoError` (it couldn't be mapped).
                 */
                Error open(const esp_partition_t &partition);

                // BlockDevice
                uint32_t     sectorSize() const override { return SectorBytes; }
                uint64_t     sectorCount() const override { return uint64_t(_blocks) * BlockBytes / SectorBytes; }
                uint32_t     eraseBlockSectors() const override { return BlockBytes / SectorBytes; }
                Error        sync() override { return flush(WaitForever); } // the queue written
                FlashDevice *flash() override { return this; }

                // FlashDevice
                uint32_t blockBytes() const override { return BlockBytes; }
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
                Error readMapped(uint32_t address, WritableByteSpan buf); // uncounted
                Error queue(FlashQueue::Kind kind, uint32_t address, ByteSpan data);
                void  work();

                static void workerMain(void *arg);

                Mutex                       _lock; // the queue
                FlashQueue                  _queue;
                Semaphore                   _work{1, 0};     // something queued
                Semaphore                   _room{16, 0};    // the worker made room (waiters recheck)
                Semaphore                   _drained{16, 0}; // the queue emptied (waiters recheck)
                Semaphore                   _ready{1, 0};    // the worker mapped the partition
                unsigned                    _roomWaiters = 0;
                unsigned                    _drainWaiters = 0;
                Task                        _worker;
                const esp_partition_t      *_partition = nullptr;
                const uint8_t              *_mapped = nullptr;
                esp_partition_mmap_handle_t _map = 0;
                Error                       _mapError;
                uint32_t                    _blocks = 0;
};

BMDS_NAMESPACE_END
