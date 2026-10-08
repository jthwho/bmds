/**
 * @file      sectorcache.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "sectorcache.h"

#include <cstring>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

Error SectorCache::init() {
        if (_slots != nullptr) return Error();
        // One block: the tags, the buckets, then the sectors (scanned or copied
        // by the CPU only, so plain Psram). The run read on a miss goes
        // straight to DMA.
        constexpr size_t TagBytes = Sectors * (sizeof(Slot) + sizeof(uint16_t));
        auto *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, TagBytes + Sectors * SectorBytes));
        auto *ahead = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, ReadAhead * SectorBytes));
        if (block == nullptr || ahead == nullptr) {
                Memory::free(block);
                Memory::free(ahead);
                return Error::NoMem;
        }
        _slots = reinterpret_cast<Slot *>(block);
        _buckets = reinterpret_cast<uint16_t *>(block + Sectors * sizeof(Slot));
        _data = block + TagBytes;
        _ahead = ahead;
        for (Ring &r : _rings) r = Ring{None, None, 0};
        for (size_t i = 0; i < Sectors; ++i) {
                _buckets[i] = None;
                _slots[i] = Slot{0, None, None, None, 0, Free, 0};
                pushHead(Free, uint16_t(i));
        }
        return Error();
}

size_t SectorCache::bucketOf(uint8_t drive, uint64_t sector) {
        // Fibonacci hashing: neighbouring sectors spread over the buckets.
        const uint64_t h = (sector ^ (uint64_t(drive) << 56)) * 0x9e3779b97f4a7c15ull;
        return size_t(h >> 56) & (Sectors - 1);
}

uint16_t SectorCache::find(uint8_t drive, uint64_t sector) const {
        if (_slots == nullptr) return None;
        for (uint16_t i = _buckets[bucketOf(drive, sector)]; i != None; i = _slots[i].hashNext)
                if (_slots[i].sector == sector && _slots[i].drive == drive) return i;
        return None;
}

void SectorCache::unlink(uint16_t i) {
        Slot &s = _slots[i];
        Ring &r = _rings[s.list];
        if (s.prev != None) {
                _slots[s.prev].next = s.next;
        } else {
                r.head = s.next;
        }
        if (s.next != None) {
                _slots[s.next].prev = s.prev;
        } else {
                r.tail = s.prev;
        }
        s.prev = s.next = None;
        --r.count;
        return;
}

void SectorCache::pushHead(List list, uint16_t i) {
        Slot &s = _slots[i];
        Ring &r = _rings[list];
        s.list = list;
        s.prev = None;
        s.next = r.head;
        if (r.head != None) {
                _slots[r.head].prev = i;
        } else {
                r.tail = i;
        }
        r.head = i;
        ++r.count;
        return;
}

void SectorCache::hashRemove(uint16_t i) {
        uint16_t *at = &_buckets[bucketOf(_slots[i].drive, _slots[i].sector)];
        while (*at != None && *at != i) at = &_slots[*at].hashNext;
        if (*at == i) *at = _slots[i].hashNext;
        _slots[i].hashNext = None;
        return;
}

void SectorCache::release(uint16_t i) {
        hashRemove(i);
        unlink(i);
        pushHead(Free, i);
        return;
}

uint16_t SectorCache::take() {
        uint16_t i = _rings[Free].tail;
        if (i == None) i = _rings[Probation].tail;
        if (i == None) i = _rings[Protected].tail;
        if (_slots[i].list != Free) hashRemove(i);
        unlink(i);
        return i;
}

void SectorCache::keep(uint8_t drive, uint64_t sector, const uint8_t *data, bool ahead) {
        uint16_t i = find(drive, sector);
        if (i == None) {
                i = take();
                Slot &s = _slots[i];
                s.drive = drive;
                s.sector = sector;
                const size_t b = bucketOf(drive, sector);
                s.hashNext = _buckets[b];
                _buckets[b] = i;
                pushHead(Probation, i);
                s.ahead = ahead ? 1 : 0;
        }
        std::memcpy(dataOf(i), data, SectorBytes);
        return;
}

bool SectorCache::read(uint8_t drive, uint64_t sector, uint8_t *out) {
        const uint16_t i = find(drive, sector);
        if (i == None) return false;
        // Used again: protected, its oldest back on probation past the limit. Read ahead, this
        // is its first use.
        unlink(i);
        if (_slots[i].ahead != 0) {
                _slots[i].ahead = 0;
                pushHead(Probation, i);
                std::memcpy(out, dataOf(i), SectorBytes);
                return true;
        }
        pushHead(Protected, i);
        if (_rings[Protected].count > ProtectedMax) {
                const uint16_t old = _rings[Protected].tail;
                unlink(old);
                pushHead(Probation, old);
        }
        std::memcpy(out, dataOf(i), SectorBytes);
        return true;
}

Error SectorCache::fill(uint8_t drive, BlockDevice &device, uint64_t sector, uint8_t *out) {
        const uint64_t total = device.sectorCount();
        const size_t   run = sector < total && total - sector < ReadAhead ? size_t(total - sector) : ReadAhead;
        if (_slots != nullptr && run > 1 && device.read(sector, WritableByteSpan(_ahead, run * SectorBytes)).isOk()) {
                // The ones after it are kept as read unless cached already (the same, written through).
                keep(drive, sector, _ahead, false);
                for (size_t k = 1; k < run; ++k)
                        if (find(drive, sector + k) == None) keep(drive, sector + k, _ahead + k * SectorBytes, true);
                std::memcpy(out, _ahead, SectorBytes);
                return Error();
        }
        // No cache, the device's last sector, or a run that failed (a bad sector after this one?).
        const Error err = device.read(sector, WritableByteSpan(out, SectorBytes));
        if (err.isOk() && _slots != nullptr) keep(drive, sector, out, false);
        return err;
}

void SectorCache::written(uint8_t drive, uint64_t sector, size_t count, const uint8_t *data) {
        if (_slots == nullptr) return;
        for (size_t k = 0; k < count; ++k) {
                const uint16_t i = find(drive, sector + k);
                if (i != None) std::memcpy(dataOf(i), data + k * SectorBytes, SectorBytes);
        }
        return;
}

void SectorCache::discard(uint8_t drive, uint64_t sector, uint64_t count) {
        if (_slots == nullptr) return;
        if (count <= Sectors) {
                for (uint64_t k = 0; k < count; ++k) {
                        const uint16_t i = find(drive, sector + k);
                        if (i != None) release(i);
                }
                return;
        }
        for (size_t i = 0; i < Sectors; ++i) {
                const Slot &s = _slots[i];
                if (s.list != Free && s.drive == drive && s.sector >= sector && s.sector - sector < count)
                        release(uint16_t(i));
        }
        return;
}

void SectorCache::discard(uint8_t drive) {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Sectors; ++i)
                if (_slots[i].list != Free && _slots[i].drive == drive) release(uint16_t(i));
        return;
}

BMDS_NAMESPACE_END
