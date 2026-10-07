/**
 * @file      ramflashdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/memory.h>
#include <bmds/ramflashdevice.h>

BMDS_NAMESPACE_BEGIN

RamFlashDevice::~RamFlashDevice() {
        Memory::free(_bytes);
}

Error RamFlashDevice::allocate(uint32_t blockBytes, uint32_t blocks, MemSpace space) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_bytes != nullptr) return Error::InvalidState;
        if (blockBytes == 0 || blockBytes % SectorBytes != 0 || blocks == 0) return Error::Invalid;
        _bytes = static_cast<uint8_t *>(Memory::alloc(space, size_t(blockBytes) * blocks));
        if (_bytes == nullptr) return Error::NoMem;
        std::memset(_bytes, 0xff, size_t(blockBytes) * blocks);
        _blockBytes = blockBytes;
        _blocks = blocks;
        return Error::Ok;
}

Error RamFlashDevice::setQueued(size_t bytes) {
        Mutex::Locker lock(_lock, WaitForever);
        const Error   err = _queue.init(bytes, MemSpace::Psram);
        if (err.isOk()) _queued = true;
        return err;
}

bool RamFlashDevice::fits(uint64_t address, size_t bytes) const {
        const uint64_t size = uint64_t(_blockBytes) * _blocks;
        return address <= size && bytes <= size - address;
}

void RamFlashDevice::apply(const FlashQueue::Op &op) {
        uint8_t *out = _bytes + op.address;
        if (op.kind == FlashQueue::Kind::Erase) {
                std::memset(out, 0xff, op.length);
        } else {
                for (uint32_t i = 0; i < op.length; ++i) out[i] &= op.data[i];
        }
        return;
}

Error RamFlashDevice::drainOneLocked() {
        FlashQueue::Op op;
        if (!_queue.front(op)) return Error::NotExist;
        apply(op);
        _queue.pop();
        return Error::Ok;
}

size_t RamFlashDevice::drain(size_t count) {
        Mutex::Locker lock(_lock, WaitForever);
        size_t        done = 0;
        while (done < count && drainOneLocked().isOk()) ++done;
        return done;
}

size_t RamFlashDevice::queued() const {
        Mutex::Locker lock(_lock, WaitForever);
        return _queue.count();
}

Error RamFlashDevice::snapshot(WritableByteSpan out) const {
        Mutex::Locker lock(_lock, WaitForever);
        if (out.size() != size_t(_blockBytes) * _blocks) return Error::Invalid;
        std::memcpy(out.data(), _bytes, out.size());
        return Error::Ok;
}

Error RamFlashDevice::load(ByteSpan image) {
        Mutex::Locker lock(_lock, WaitForever);
        if (image.size() != size_t(_blockBytes) * _blocks) return Error::Invalid;
        std::memcpy(_bytes, image.data(), image.size());
        while (!_queue.isEmpty()) _queue.pop();
        return Error::Ok;
}

Error RamFlashDevice::read(uint32_t address, WritableByteSpan buf) {
        Mutex::Locker lock(_lock, WaitForever);
        if (!fits(address, buf.size())) return Error::OutOfRange;
        std::memcpy(buf.data(), _bytes + address, buf.size());
        if (_queued) _queue.overlay(address, buf);
        return Error::Ok;
}

Error RamFlashDevice::program(uint32_t address, ByteSpan data) {
        Mutex::Locker lock(_lock, WaitForever);
        if (!fits(address, data.size())) return Error::OutOfRange;
        while (!data.isEmpty()) {
                const size_t n = _queued && data.size() > _queue.maxProgram() ? _queue.maxProgram() : data.size();
                if (!_queued) {
                        apply(FlashQueue::Op{FlashQueue::Kind::Program, address, uint32_t(n), data.data()});
                } else {
                        // No room: do the oldest, as a worker would before the writer could go on.
                        while (_queue.tryProgram(address, ByteSpan(data.data(), n)) == Error::Full)
                                (void)drainOneLocked();
                }
                address += uint32_t(n);
                data = ByteSpan(data.data() + n, data.size() - n);
        }
        return Error::Ok;
}

Error RamFlashDevice::erase(uint32_t block) {
        Mutex::Locker lock(_lock, WaitForever);
        if (block >= _blocks) return Error::OutOfRange;
        const FlashQueue::Op op{FlashQueue::Kind::Erase, block * _blockBytes, _blockBytes, nullptr};
        if (!_queued) {
                apply(op);
                return Error::Ok;
        }
        while (_queue.tryErase(op.address, op.length) == Error::Full) (void)drainOneLocked();
        return Error::Ok;
}

Error RamFlashDevice::flush(unsigned int) {
        Mutex::Locker lock(_lock, WaitForever);
        while (drainOneLocked().isOk()) {}
        return Error::Ok;
}

Error RamFlashDevice::readSectors(uint64_t sector, WritableByteSpan buf) {
        return read(static_cast<uint32_t>(sector * SectorBytes), buf);
}

BMDS_NAMESPACE_END
