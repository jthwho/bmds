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
        // One block: the tags, then the sectors (both scanned or copied by the
        // CPU only, so plain Psram).
        auto *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Sectors * (sizeof(Slot) + SectorBytes)));
        if (block == nullptr) return Error::NoMem;
        _slots = reinterpret_cast<Slot *>(block);
        _data = block + Sectors * sizeof(Slot);
        for (size_t i = 0; i < Sectors; ++i) _slots[i] = Slot{0, 0, NoDrive};
        return Error();
}

SectorCache::Slot *SectorCache::find(uint8_t drive, uint64_t sector) {
        if (_slots == nullptr) return nullptr;
        for (size_t i = 0; i < Sectors; ++i) {
                Slot &s = _slots[i];
                if (s.drive == drive && s.sector == sector) return &s;
        }
        return nullptr;
}

bool SectorCache::read(uint8_t drive, uint64_t sector, uint8_t *out) {
        Slot *s = find(drive, sector);
        if (s == nullptr) return false;
        s->used = ++_clock;
        std::memcpy(out, dataOf(*s), SectorBytes);
        return true;
}

void SectorCache::store(uint8_t drive, uint64_t sector, const uint8_t *data) {
        if (_slots == nullptr) return;
        Slot *s = find(drive, sector);
        if (s == nullptr) {
                // An empty slot, else the least recently used (by age, so the
                // clock wrapping around costs one poor choice, not a wrong one).
                s = &_slots[0];
                for (size_t i = 0; i < Sectors && s->drive != NoDrive; ++i) {
                        Slot &c = _slots[i];
                        if (c.drive == NoDrive || _clock - c.used > _clock - s->used) s = &c;
                }
                s->drive = drive;
                s->sector = sector;
        }
        s->used = ++_clock;
        std::memcpy(dataOf(*s), data, SectorBytes);
        return;
}

void SectorCache::written(uint8_t drive, uint64_t sector, size_t count, const uint8_t *data) {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Sectors; ++i) {
                Slot &s = _slots[i];
                if (s.drive != drive || s.sector < sector || s.sector - sector >= count) continue;
                std::memcpy(dataOf(s), data + (s.sector - sector) * SectorBytes, SectorBytes);
        }
        return;
}

void SectorCache::discard(uint8_t drive, uint64_t sector, uint64_t count) {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Sectors; ++i) {
                Slot &s = _slots[i];
                if (s.drive == drive && s.sector >= sector && s.sector - sector < count) s.drive = NoDrive;
        }
        return;
}

void SectorCache::discard(uint8_t drive) {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Sectors; ++i)
                if (_slots[i].drive == drive) _slots[i].drive = NoDrive;
        return;
}

BMDS_NAMESPACE_END
