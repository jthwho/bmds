/**
 * @file      blockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/blockdevice.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

Error BlockDevice::checkRequest(uint64_t sector, size_t bytes) const {
        if (!isPresent()) return Error::NoMedia;
        const uint32_t size = sectorSize();
        if (bytes == 0 || size == 0 || bytes % size != 0) return Error::Invalid;
        const uint64_t count = bytes / size;
        const uint64_t total = sectorCount();
        if (sector >= total || count > total - sector) return Error::OutOfRange;
        return Error();
}

Error BlockDevice::read(uint64_t sector, WritableByteSpan buf) {
        Error err = checkRequest(sector, buf.size());
        if (err.isError()) return err;
        const int64_t started = Detail::monotonicUs();
        err = readSectors(sector, buf);
        countRequest(false, buf.size(), err, started);
        return err;
}

Error BlockDevice::write(uint64_t sector, ByteSpan data) {
        Error err = checkRequest(sector, data.size());
        if (err.isError()) return err;
        const int64_t started = Detail::monotonicUs();
        err = writeSectors(sector, data);
        countRequest(true, data.size(), err, started);
        return err;
}

void BlockDevice::countRequest(bool write, size_t bytes, Error err, int64_t startedUs) {
        const int64_t us = Detail::monotonicUs() - startedUs;
        (write ? _writes : _reads).fetch_add(1, std::memory_order_relaxed);
        if (err.isOk()) (write ? _writtenBytes : _readBytes).fetch_add(bytes, std::memory_order_relaxed);
        if (err.isError() && err != Error::NoMedia) _errors.fetch_add(1, std::memory_order_relaxed);
        _ioUs.fetch_add(uint64_t(us > 0 ? us : 0), std::memory_order_relaxed);
        return;
}

BlockDevice::IoStats BlockDevice::ioStats() const {
        IoStats s;
        s.reads = _reads.load(std::memory_order_relaxed);
        s.writes = _writes.load(std::memory_order_relaxed);
        s.readBytes = _readBytes.load(std::memory_order_relaxed);
        s.writtenBytes = _writtenBytes.load(std::memory_order_relaxed);
        s.errors = _errors.load(std::memory_order_relaxed);
        s.ioUs = _ioUs.load(std::memory_order_relaxed);
        s.erases = _erases.load(std::memory_order_relaxed);
        return s;
}

Error BlockDevice::trim(uint64_t, uint64_t) {
        return Error::NotSupported;
}

BMDS_NAMESPACE_END
