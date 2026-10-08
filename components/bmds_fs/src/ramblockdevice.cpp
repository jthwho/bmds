/**
 * @file      ramblockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <algorithm>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/ramblockdevice.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool allZero(const uint8_t *p, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                        if (p[i] != 0) return false;
                }
                return true;
        }

} // namespace

RamBlockDevice::RamBlockDevice(uint32_t sectorSize)
    : _sectorSize(sectorSize),
      _chunkSectors(sectorSize == 0 || sectorSize >= ChunkBytes ? 1 : static_cast<uint32_t>(ChunkBytes / sectorSize)) {}

RamBlockDevice::~RamBlockDevice() {
        freeChunks();
        return;
}

void RamBlockDevice::freeChunks() {
        for (size_t i = 0; i < _chunkCount; ++i) Memory::free(_chunks[i]);
        Memory::free(_chunks);
        _chunks = nullptr;
        _chunkCount = 0;
        _usedChunks = 0;
        _sectors = 0;
        return;
}

Error RamBlockDevice::allocate(uint64_t sectorCount, MemSpace space) {
        if (sectorCount == 0 || _sectorSize == 0 || sectorCount > SIZE_MAX / _sectorSize) return Error::Invalid;
        const size_t count = static_cast<size_t>((sectorCount + _chunkSectors - 1) / _chunkSectors);
        auto        *chunks = static_cast<uint8_t **>(Memory::alloc(space, count * sizeof(uint8_t *)));
        if (chunks == nullptr) return Error::NoMem;
        for (size_t i = 0; i < count; ++i) chunks[i] = nullptr;
        Mutex::Locker lock(_lock, WaitForever);
        freeChunks();
        _chunks = chunks;
        _chunkCount = count;
        _sectors = sectorCount;
        _space = space;
        return Error();
}

void RamBlockDevice::setPresent(bool present) {
        Mutex::Locker lock(_lock, WaitForever);
        _present = present;
        return;
}

size_t RamBlockDevice::usedBytes() const {
        Mutex::Locker lock(_lock, WaitForever);
        return _usedChunks * _chunkSectors * _sectorSize;
}

// Unlocked: every request asks (BlockDevice::checkRequest()), and readSectors() checks again.
uint64_t RamBlockDevice::sectorCount() const {
        return _present.load(std::memory_order_acquire) ? _sectors.load(std::memory_order_acquire) : 0;
}

bool RamBlockDevice::isPresent() const {
        return _present.load(std::memory_order_acquire);
}

Error RamBlockDevice::sync() {
        Mutex::Locker lock(_lock, WaitForever);
        return _present ? Error() : Error::NoMedia;
}

Error RamBlockDevice::trim(uint64_t first, uint64_t count) {
        Mutex::Locker lock(_lock, WaitForever);
        const uint64_t total = _sectors.load(std::memory_order_relaxed); // set under the lock
        if (!_present) return Error::NoMedia;
        if (first >= total || count > total - first) return Error::OutOfRange;
        // Trimmed sectors become zeros; a chunk left all zeros is freed (a
        // file's first and last chunks are usually shared with its neighbors).
        const uint64_t end = first + count;
        for (uint64_t c = first / _chunkSectors; c < _chunkCount && c * _chunkSectors < end; ++c) {
                uint8_t *chunk = _chunks[c];
                if (chunk == nullptr) continue;
                const uint64_t start = c * _chunkSectors;
                const uint64_t from = std::max(first, start);
                const uint64_t to = std::min({end, start + _chunkSectors, total});
                const size_t   chunkBytes = size_t(_chunkSectors) * _sectorSize;
                const bool     whole = from == start && to == std::min(start + _chunkSectors, total);
                if (!whole) {
                        std::memset(chunk + size_t(from - start) * _sectorSize, 0, size_t(to - from) * _sectorSize);
                        if (!allZero(chunk, chunkBytes)) continue;
                }
                Memory::free(chunk);
                _chunks[c] = nullptr;
                --_usedChunks;
        }
        return Error();
}

Error RamBlockDevice::readSectors(uint64_t sector, WritableByteSpan buf) {
        Mutex::Locker lock(_lock, WaitForever);
        const uint64_t total = _sectors.load(std::memory_order_relaxed); // set under the lock
        // Removed (or resized) since checkRequest().
        if (!_present) return Error::NoMedia;
        const uint64_t n = buf.size() / _sectorSize;
        if (sector > total || n > total - sector) return Error::OutOfRange;
        uint8_t *out = buf.data();
        while (out < buf.data() + buf.size()) {
                const size_t   c = static_cast<size_t>(sector / _chunkSectors);
                const uint32_t within = static_cast<uint32_t>(sector % _chunkSectors);
                const size_t   left = static_cast<size_t>(buf.data() + buf.size() - out);
                const size_t   piece = std::min<size_t>(left, size_t(_chunkSectors - within) * _sectorSize);
                if (_chunks[c] == nullptr) {
                        std::memset(out, 0, piece);
                } else {
                        std::memcpy(out, _chunks[c] + size_t(within) * _sectorSize, piece);
                }
                out += piece;
                sector += piece / _sectorSize;
        }
        return Error();
}

Error RamBlockDevice::writeSectors(uint64_t sector, ByteSpan data) {
        Mutex::Locker lock(_lock, WaitForever);
        const uint64_t total = _sectors.load(std::memory_order_relaxed); // set under the lock
        if (!_present) return Error::NoMedia;
        const uint64_t n = data.size() / _sectorSize;
        if (sector > total || n > total - sector) return Error::OutOfRange;
        const uint8_t *in = data.data();
        const size_t   chunkBytes = size_t(_chunkSectors) * _sectorSize;
        while (in < data.data() + data.size()) {
                const size_t   c = static_cast<size_t>(sector / _chunkSectors);
                const uint32_t within = static_cast<uint32_t>(sector % _chunkSectors);
                const size_t   left = static_cast<size_t>(data.data() + data.size() - in);
                const size_t   piece = std::min<size_t>(left, size_t(_chunkSectors - within) * _sectorSize);
                if (_chunks[c] == nullptr && !allZero(in, piece)) {
                        auto *chunk = static_cast<uint8_t *>(Memory::alloc(_space, chunkBytes));
                        if (chunk == nullptr) return Error::NoMem;
                        std::memset(chunk, 0, chunkBytes);
                        _chunks[c] = chunk;
                        ++_usedChunks;
                }
                if (_chunks[c] != nullptr) std::memcpy(_chunks[c] + size_t(within) * _sectorSize, in, piece);
                in += piece;
                sector += piece / _sectorSize;
        }
        return Error();
}

BMDS_NAMESPACE_END
