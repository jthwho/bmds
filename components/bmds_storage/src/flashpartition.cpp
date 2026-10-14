/**
 * @file      flashpartition.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A flash partition with an ordered write cache, for littlefs
 * (devplan/storage.md §13d).
 */

#include <cstring>
#include <sdkconfig.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/flashwrites.h>
#include <bmds/log.h>
#include <bmds/memspace.h>

#include "flashpartition.h"

BMDS_NAMESPACE_BEGIN

Error FlashPartition::open(const esp_partition_t &partition) {
        if (_partition != nullptr) return Error::InvalidState;
        if (partition.size % BlockBytes != 0 || partition.address % BlockBytes != 0) return Error::Invalid;
        Error err = _queue.init(QueueBytes, MemSpace::Psram);
        if (err.isError()) return err;
        _partition = &partition;
        _blocks = static_cast<uint32_t>(partition.size / BlockBytes);
        Task::Config config;
        config.name = "flash0";
        config.stackBytes = 3072;
        config.stackSpace = MemSpace::Internal; // a flash operation needs it
        config.priority = CONFIG_BMDS_STORAGE_PRIORITY;
        err = _worker.start(config, workerMain, this);
        if (err.isError()) return err;
        // Mapping changes the MMU with the caches off, which needs the worker's internal stack too.
        (void)_ready.acquire(WaitForever);
        return _mapError;
}

bool FlashPartition::fits(uint64_t address, size_t bytes) const {
        const uint64_t size = uint64_t(_blocks) * BlockBytes;
        return _mapped != nullptr && address <= size && bytes <= size - address;
}

Error FlashPartition::read(uint32_t address, WritableByteSpan buf) {
        const int64_t started = Detail::monotonicUs();
        const Error   err = readMapped(address, buf);
        countRequest(false, buf.size(), err, started); // littlefs's reads (devplan/sysmon.md §6)
        return err;
}

Error FlashPartition::readMapped(uint32_t address, WritableByteSpan buf) {
        if (!fits(address, buf.size())) return Error::OutOfRange;
        Mutex::Locker lock(_lock, WaitForever);
        std::memcpy(buf.data(), _mapped + address, buf.size());
        _queue.overlay(address, buf);
        return Error::Ok;
}

Error FlashPartition::readSectors(uint64_t sector, WritableByteSpan buf) {
        return readMapped(static_cast<uint32_t>(sector * SectorBytes), buf); // counted by BlockDevice::read()
}

// One record into the queue, waiting for room as long as it takes.
Error FlashPartition::queue(FlashQueue::Kind kind, uint32_t address, ByteSpan data) {
        for (;;) {
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        const Error   err = kind == FlashQueue::Kind::Erase ? _queue.tryErase(address, BlockBytes)
                                                                            : _queue.tryProgram(address, data);
                        if (err != Error::Full) {
                                if (err.isOk()) _work.release();
                                return err;
                        }
                        ++_roomWaiters;
                }
                _work.release();
                (void)_room.acquire(WaitForever);
        }
}

Error FlashPartition::program(uint32_t address, ByteSpan data) {
        if (!fits(address, data.size())) return Error::OutOfRange;
        const int64_t started = Detail::monotonicUs();
        const size_t  bytes = data.size();
        Error         err = Error::Ok;
        while (!data.isEmpty() && err.isOk()) {
                const size_t n = data.size() < _queue.maxProgram() ? data.size() : _queue.maxProgram();
                err = queue(FlashQueue::Kind::Program, address, ByteSpan(data.data(), n));
                address += static_cast<uint32_t>(n);
                data = ByteSpan(data.data() + n, data.size() - n);
        }
        countRequest(true, bytes, err, started); // queued: the time is the wait for room
        return err;
}

Error FlashPartition::erase(uint32_t block) {
        if (block >= _blocks) return Error::OutOfRange;
        const Error err = queue(FlashQueue::Kind::Erase, block * BlockBytes, ByteSpan());
        if (err.isOk()) countErase();
        return err;
}

Error FlashPartition::flush(unsigned int timeoutMs) {
        const Deadline deadline(timeoutMs);
        for (;;) {
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        if (_queue.isEmpty()) return Error::Ok;
                        ++_drainWaiters;
                }
                _work.release();
                if (_drained.acquire(deadline).isError()) return Error::Timeout;
        }
}

void FlashPartition::workerMain(void *arg) {
        static_cast<FlashPartition *>(arg)->work();
        return;
}

// The queue, oldest first, a record at a time: done on the flash, then removed (a read in
// between lays it over the flash again, which gives the same bytes).
void FlashPartition::work() {
        const void     *mapped = nullptr;
        const esp_err_t mapErr =
                esp_partition_mmap(_partition, 0, _partition->size, ESP_PARTITION_MMAP_DATA, &mapped, &_map);
        _mapError = mapErr == ESP_OK ? Error() : Error(Error::IoError);
        _mapped = static_cast<const uint8_t *>(mapped);
        _ready.release();
        if (mapErr != ESP_OK) {
                for (;;) Task::sleep(1000000); // nothing to do: open() failed
        }
        // The last hold given back wakes the worker as new work does (devplan/cfg.md §3).
        WaitList::Entry released(FlashWrites::releases(), _work);
        for (;;) {
                (void)_work.acquire(WaitForever);
                // Let the writer finish (a close is several operations): start once it has
                // paused, at the latest MaxDelayMs on, at once when somebody waits.
                const Deadline latest(MaxDelayMs);
                for (;;) {
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                if (_drainWaiters != 0 || _roomWaiters != 0 || _queue.isHalfFull()) break;
                        }
                        if (latest.isExpired() || _work.acquire(DelayMs).isError()) break;
                }
                // Held (audio playing): the queue waits until the hold goes, somebody waits for it
                // (sync, a restart), or it is half full. Reads see it meanwhile.
                for (;;) {
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                if (_queue.isEmpty() || _drainWaiters != 0 || _roomWaiters != 0 ||
                                    _queue.isHalfFull() || !FlashWrites::isHeld())
                                        break;
                        }
                        (void)_work.acquire(WaitForever);
                }
                for (;;) {
                        FlashQueue::Op op;
                        {
                                Mutex::Locker lock(_lock, WaitForever);
                                if (!_queue.front(op)) {
                                        for (; _drainWaiters != 0; --_drainWaiters) _drained.release();
                                        break;
                                }
                        }
                        const esp_err_t err = op.kind == FlashQueue::Kind::Erase
                                                      ? esp_partition_erase_range(_partition, op.address, op.length)
                                                      : esp_partition_write(_partition, op.address, op.data, op.length);
                        // littlefs can't move off a bad block it was told was fine: logged, and on.
                        if (err != ESP_OK)
                                bmdsErr("flash0: %s at 0x%lx failed: %s",
                                        op.kind == FlashQueue::Kind::Erase ? "erase" : "program",
                                        static_cast<unsigned long>(op.address), esp_err_to_name(err));
                        Mutex::Locker lock(_lock, WaitForever);
                        _queue.pop();
                        for (; _roomWaiters != 0; --_roomWaiters) _room.release();
                }
        }
}

BMDS_NAMESPACE_END
