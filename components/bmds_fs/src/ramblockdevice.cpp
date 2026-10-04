/**
 * @file      ramblockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/memory.h>
#include <bmds/ramblockdevice.h>

BMDS_NAMESPACE_BEGIN

RamBlockDevice::RamBlockDevice(uint32_t sectorSize) : _sectorSize(sectorSize) {}

RamBlockDevice::~RamBlockDevice() {
        Memory::free(_data);
}

Error RamBlockDevice::allocate(uint64_t sectorCount, MemSpace space) {
        if (sectorCount == 0 || _sectorSize == 0 || sectorCount > SIZE_MAX / _sectorSize) return Error::Invalid;
        const size_t bytes = static_cast<size_t>(sectorCount) * _sectorSize;
        auto        *data = static_cast<uint8_t *>(Memory::alloc(space, bytes));
        if (data == nullptr) return Error::NoMem;
        std::memset(data, 0, bytes);
        Mutex::Locker lock(_lock, WaitForever);
        Memory::free(_data);
        _data = data;
        _bytes = bytes;
        return Error();
}

void RamBlockDevice::setPresent(bool present) {
        Mutex::Locker lock(_lock, WaitForever);
        _present = present;
        return;
}

uint64_t RamBlockDevice::sectorCount() const {
        Mutex::Locker lock(_lock, WaitForever);
        return _present ? _bytes / _sectorSize : 0;
}

bool RamBlockDevice::isPresent() const {
        Mutex::Locker lock(_lock, WaitForever);
        return _present;
}

Error RamBlockDevice::sync() {
        Mutex::Locker lock(_lock, WaitForever);
        return _present ? Error() : Error::NoMedia;
}

Error RamBlockDevice::trim(uint64_t first, uint64_t count) {
        Mutex::Locker lock(_lock, WaitForever);
        if (!_present) return Error::NoMedia;
        const uint64_t total = _bytes / _sectorSize;
        if (first >= total || count > total - first) return Error::OutOfRange;
        return Error();
}

Error RamBlockDevice::readSectors(uint64_t sector, WritableByteSpan buf) {
        Mutex::Locker lock(_lock, WaitForever);
        // Removed (or resized) since checkRequest().
        if (!_present) return Error::NoMedia;
        if (sector * _sectorSize + buf.size() > _bytes) return Error::OutOfRange;
        std::memcpy(buf.data(), _data + sector * _sectorSize, buf.size());
        return Error();
}

Error RamBlockDevice::writeSectors(uint64_t sector, ByteSpan data) {
        Mutex::Locker lock(_lock, WaitForever);
        if (!_present) return Error::NoMedia;
        if (sector * _sectorSize + data.size() > _bytes) return Error::OutOfRange;
        std::memcpy(_data + sector * _sectorSize, data.data(), data.size());
        return Error();
}

BMDS_NAMESPACE_END
