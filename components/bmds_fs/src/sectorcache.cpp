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
        // Then the held sectors' originals and a sector for phase 1 (devplan/storage.md §3).
        constexpr size_t TagBytes = Sectors * (sizeof(Slot) + sizeof(uint16_t)) + MaxHeld * sizeof(uint16_t);
        constexpr size_t Bytes = TagBytes + (Sectors + 2 * MaxHeld + 1) * SectorBytes;
        auto            *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes));
        auto *ahead = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, ReadAhead * SectorBytes));
        if (block == nullptr || ahead == nullptr) {
                Memory::free(block);
                Memory::free(ahead);
                return Error::NoMem;
        }
        _slots = reinterpret_cast<Slot *>(block);
        _buckets = reinterpret_cast<uint16_t *>(block + Sectors * sizeof(Slot));
        _held = _buckets + Sectors;
        _data = block + TagBytes;
        _originals = _data + Sectors * SectorBytes;
        _freed = _originals + MaxHeld * SectorBytes;
        _merged = _freed + MaxHeld * SectorBytes;
        _heldCount = 0;
        _freesHeld = 0;
        _ahead = ahead;
        for (Ring &r : _rings) r = Ring{None, None, 0};
        for (size_t i = 0; i < Sectors; ++i) {
                _buckets[i] = None;
                _slots[i] = Slot{0, None, None, None, 0, Free, 0, 0, 0};
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
        if (const int at = heldIndex(i); at >= 0) unhold(size_t(at)); // dropped: trimmed, or the drive gone
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
        if (_slots[i].list == Held) {
                std::memcpy(out, dataOf(i), SectorBytes); // the newest, not written yet
                return true;
        }
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

// ---- Writing back (devplan/storage.md §3)

int SectorCache::heldIndex(uint16_t i) const {
        if (_slots[i].list != Held) return -1;
        for (size_t at = 0; at < _heldCount; ++at)
                if (_held[at] == i) return int(at);
        return -1;
}

bool SectorCache::entryUsed(Kind kind, const uint8_t *sector, size_t entry) {
        switch (kind) {
                case Kind::Bitmap: return (sector[entry / 8] >> (entry % 8) & 1) != 0;
                case Kind::Fat16: return (sector[entry * 2] | sector[entry * 2 + 1]) != 0;
                case Kind::Fat32:
                        return (sector[entry * 4] | sector[entry * 4 + 1] | sector[entry * 4 + 2] |
                                (sector[entry * 4 + 3] & 0x0f)) != 0;
                default: return false;
        }
}

// The entries _held[at] frees (used in its original, free now) added to its mask, which keeps them
// until the flush even if used again: a cluster freed and reused within the window is still the
// old file's on the card. @return True if the mask has any.
bool SectorCache::noteFrees(size_t at) {
        const uint16_t i = _held[at];
        const auto     kind = Kind(_slots[i].kind);
        const size_t   entries = kind == Kind::Bitmap  ? SectorBytes * 8
                                 : kind == Kind::Fat16 ? SectorBytes / 2
                                 : kind == Kind::Fat32 ? SectorBytes / 4
                                                       : 0;
        const uint8_t *was = _originals + at * SectorBytes;
        const uint8_t *now = _data + size_t(i) * SectorBytes;
        uint8_t       *mask = _freed + at * SectorBytes;
        bool           any = false;
        for (size_t e = 0; e < entries; ++e) {
                if (entryUsed(kind, was, e) && !entryUsed(kind, now, e)) mask[e / 8] |= uint8_t(1u << (e % 8));
                any = any || (mask[e / 8] >> (e % 8) & 1) != 0;
        }
        return any;
}

bool SectorCache::isFreeHeld(uint8_t drive, uint64_t sector, size_t entry) const {
        const uint16_t i = find(drive, sector);
        if (i == None || _slots[i].frees == 0) return false;
        const int at = heldIndex(i);
        if (at < 0) return false;
        return (_freed[size_t(at) * SectorBytes + entry / 8] >> (entry % 8) & 1) != 0;
}

// _held[at] clean again (on probation), the last held taking its place.
void SectorCache::unhold(size_t at) {
        const uint16_t i = _held[at];
        if (_slots[i].frees != 0) {
                _slots[i].frees = 0;
                --_freesHeld;
        }
        const size_t last = --_heldCount;
        if (at != last) {
                _held[at] = _held[last];
                std::memcpy(_originals + at * SectorBytes, _originals + last * SectorBytes, SectorBytes);
                std::memcpy(_freed + at * SectorBytes, _freed + last * SectorBytes, SectorBytes);
        }
        unlink(i);
        pushHead(Probation, i);
        return;
}

bool SectorCache::hasHeld(uint8_t drive) const {
        for (size_t at = 0; at < _heldCount; ++at)
                if (_slots[_held[at]].drive == drive) return true;
        return false;
}

Error SectorCache::hold(uint8_t drive, BlockDevice &device, uint64_t sector, const uint8_t *data, Kind kind) {
        if (_slots == nullptr) return Error::NoMem;
        uint16_t i = find(drive, sector);
        if (i == None || _slots[i].list != Held) {
                if (_heldCount == MaxHeld) return Error::Full;
                uint8_t *original = _originals + _heldCount * SectorBytes;
                if (kind == Kind::Fat16 || kind == Kind::Fat32 || kind == Kind::Bitmap) {
                        // What the card has now: the clean copy (FatFs reads a FAT or bitmap sector before
                        // changing it), else read.
                        if (i != None) {
                                std::memcpy(original, dataOf(i), SectorBytes);
                        } else {
                                const Error err = device.read(sector, WritableByteSpan(original, SectorBytes));
                                if (err.isError()) return err;
                        }
                }
                if (i == None) {
                        i = take();
                        Slot &s = _slots[i];
                        s.drive = drive;
                        s.sector = sector;
                        s.ahead = 0;
                        const size_t b = bucketOf(drive, sector);
                        s.hashNext = _buckets[b];
                        _buckets[b] = i;
                } else {
                        unlink(i);
                }
                pushHead(Held, i);
                std::memset(_freed + _heldCount * SectorBytes, 0, SectorBytes);
                _held[_heldCount++] = i;
        }
        _slots[i].kind = uint8_t(kind) & 7;
        std::memcpy(dataOf(i), data, SectorBytes);
        // Whether it frees anything used on the card: those clusters wait for the flush.
        const bool frees =
                (kind == Kind::Fat16 || kind == Kind::Fat32 || kind == Kind::Bitmap) && noteFrees(size_t(heldIndex(i)));
        if (frees != (_slots[i].frees != 0)) {
                _slots[i].frees = frees ? 1 : 0;
                if (frees) {
                        ++_freesHeld;
                } else {
                        --_freesHeld;
                }
        }
        return Error();
}

// What _held[at] is written as in @p phase (1: allocation, nothing freed; 2: directories; 3: the
// rest as it is), or null if it isn't written then.
const uint8_t *SectorCache::phaseData(size_t at, int phase) {
        const uint16_t i = _held[at];
        const auto     kind = Kind(_slots[i].kind);
        const bool     allocation = kind == Kind::Fat16 || kind == Kind::Fat32 || kind == Kind::Bitmap;
        const uint8_t *now = dataOf(i);
        const uint8_t *was = _originals + at * SectorBytes;
        if (phase == 2) return kind == Kind::Directory ? now : nullptr;
        if (!allocation) return phase == 3 && kind == Kind::Other ? now : nullptr;
        // Phase 1: every cluster used before or now marked used (a bitmap OR'd; a FAT entry freed
        // kept as it was). Phase 3: as it is, if that differs.
        std::memcpy(_merged, now, SectorBytes);
        if (kind == Kind::Bitmap) {
                for (size_t b = 0; b < SectorBytes; ++b) _merged[b] |= was[b];
        } else if (kind == Kind::Fat16) {
                for (size_t b = 0; b < SectorBytes; b += 2)
                        if ((now[b] | now[b + 1]) == 0) std::memcpy(_merged + b, was + b, 2);
        } else {
                for (size_t b = 0; b < SectorBytes; b += 4) {
                        const uint32_t n = uint32_t(now[b]) | uint32_t(now[b + 1]) << 8 | uint32_t(now[b + 2]) << 16 |
                                           uint32_t(now[b + 3] & 0x0f) << 24;
                        if (n == 0) std::memcpy(_merged + b, was + b, 4);
                }
        }
        if (phase == 1) return std::memcmp(_merged, was, SectorBytes) != 0 ? _merged : nullptr;
        return std::memcmp(now, _merged, SectorBytes) != 0 ? now : nullptr;
}

// @p order's sectors (ascending) as @p phase writes them, neighbours in one request.
Error SectorCache::writeRun(uint8_t drive, BlockDevice &device, const uint16_t *order, size_t count, int phase) {
        size_t   run = 0;
        uint64_t first = 0;
        auto     put = [&]() {
                const Error err = run == 0 ? Error() : device.write(first, ByteSpan(_ahead, run * SectorBytes));
                run = 0;
                return err;
        };
        for (size_t n = 0; n < count; ++n) {
                const size_t at = order[n];
                if (_slots[_held[at]].drive != drive) continue;
                const uint8_t *bytes = phaseData(at, phase);
                if (bytes == nullptr) continue;
                const uint64_t sector = _slots[_held[at]].sector;
                if (run > 0 && (sector != first + run || run == ReadAhead)) {
                        const Error err = put();
                        if (err.isError()) return err;
                }
                if (run == 0) first = sector;
                std::memcpy(_ahead + run * SectorBytes, bytes, SectorBytes);
                ++run;
        }
        return put();
}

Error SectorCache::flush(uint8_t drive, BlockDevice &device) {
        if (_slots == nullptr || !hasHeld(drive)) return Error();
        // The held, by sector (insertion: at most MaxHeld).
        uint16_t order[MaxHeld];
        size_t   n = 0;
        for (size_t at = 0; at < _heldCount; ++at) {
                size_t j = n++;
                for (; j > 0 && _slots[_held[order[j - 1]]].sector > _slots[_held[at]].sector; --j)
                        order[j] = order[j - 1];
                order[j] = uint16_t(at);
        }
        Error err;
        for (int phase = 1; phase <= 3 && err.isOk(); ++phase) err = writeRun(drive, device, order, n, phase);
        // Written (or lost with the card): clean copies now, or none.
        for (size_t at = _heldCount; at-- > 0;) {
                const uint16_t i = _held[at];
                if (_slots[i].drive != drive) continue;
                unhold(at);
                if (err.isError()) release(i);
        }
        return err;
}

void SectorCache::written(uint8_t drive, uint64_t sector, size_t count, const uint8_t *data) {
        if (_slots == nullptr) return;
        for (size_t k = 0; k < count; ++k) {
                const uint16_t i = find(drive, sector + k);
                if (i == None) continue;
                std::memcpy(dataOf(i), data + k * SectorBytes, SectorBytes);
                // Held, and written past: the device has the newest bytes now.
                if (const int at = heldIndex(i); at >= 0) unhold(size_t(at));
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
