/**
 * @file      rangeheap.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/rangeheap.h>

#include <cstring>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t MinRecords = 16;
        constexpr uint32_t MinBuckets = 16;

        unsigned msb(uint32_t x) {
                return 31u - unsigned(__builtin_clz(x));
        }

        bool isPowerOfTwo(uint32_t x) {
                return x != 0 && (x & (x - 1)) == 0;
        }

} // namespace

RangeHeap::RangeHeap() {
        for (auto &fl : _lists)
                for (uint32_t &head : fl) head = None;
        return;
}

RangeHeap::~RangeHeap() {
        Memory::free(_nodes);
        Memory::free(_buckets);
        return;
}

// ---- Records

Error RangeHeap::reserve(uint32_t records) {
        if (_spares >= records) return Error::Ok;
        uint32_t grown = _capacity < MinRecords ? MinRecords : _capacity * 2;
        if (grown < _capacity + records) grown = _capacity + records;
        auto *nodes = static_cast<Node *>(Memory::alloc(MemSpace::Psram, size_t(grown) * sizeof(Node)));
        if (nodes == nullptr) return Error::NoMem;
        if (_nodes != nullptr) std::memcpy(nodes, _nodes, size_t(_capacity) * sizeof(Node));
        Memory::free(_nodes);
        _nodes = nodes;
        for (uint32_t i = grown; i > _capacity; --i) {
                Node &n = _nodes[i - 1];
                n = Node{};
                n.owner = SpareOwner;
                n.freePrev = _spare;
                _spare = i - 1;
        }
        _spares += grown - _capacity;
        _capacity = grown;
        return Error::Ok;
}

uint32_t RangeHeap::takeRecord() {
        const uint32_t i = _spare;
        _spare = _nodes[i].freePrev;
        --_spares;
        return i;
}

void RangeHeap::dropRecord(uint32_t i) {
        _nodes[i].owner = SpareOwner;
        _nodes[i].freePrev = _spare;
        _spare = i;
        ++_spares;
        return;
}

// ---- The hash: every record in use, by its start

uint32_t RangeHeap::bucketOf(uint32_t at) const {
        return _hashShift >= 32 ? 0 : ((at / Unit) * 2654435761u) >> _hashShift;
}

Error RangeHeap::growHash() {
        const uint32_t count = _bucketCount < MinBuckets ? MinBuckets : _bucketCount * 2;
        auto *buckets = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, size_t(count) * sizeof(uint32_t)));
        if (buckets == nullptr) return Error::NoMem;
        Memory::free(_buckets);
        _buckets = buckets;
        _bucketCount = count;
        _hashShift = 32 - msb(count);
        for (uint32_t b = 0; b < count; ++b) _buckets[b] = None;
        _hashed = 0;
        for (uint32_t i = 0; i < _capacity; ++i)
                if (_nodes[i].owner != SpareOwner) hashInsert(i);
        return Error::Ok;
}

void RangeHeap::hashInsert(uint32_t i) {
        const uint32_t b = bucketOf(_nodes[i].at);
        _nodes[i].hashNext = _buckets[b];
        _buckets[b] = i;
        ++_hashed;
        return;
}

void RangeHeap::hashRemove(uint32_t i) {
        for (uint32_t *at = &_buckets[bucketOf(_nodes[i].at)]; *at != None; at = &_nodes[*at].hashNext) {
                if (*at != i) continue;
                *at = _nodes[i].hashNext;
                --_hashed;
                return;
        }
        return;
}

uint32_t RangeHeap::find(uint32_t at) const {
        if (_bucketCount == 0) return None;
        for (uint32_t i = _buckets[bucketOf(at)]; i != None; i = _nodes[i].hashNext)
                if (_nodes[i].at == at) return i;
        return None;
}

// ---- The free lists (TLSF's mapping: a class per quarter of a power of two)

void RangeHeap::listInsert(uint32_t i) {
        const uint32_t size = _nodes[i].size;
        const unsigned fl = msb(size);
        const unsigned sl = (size >> (fl - SlBits)) & (SlCount - 1);
        uint32_t      &head = _lists[fl - FlShift][sl];
        _nodes[i].freePrev = None;
        _nodes[i].freeNext = head;
        if (head != None) _nodes[head].freePrev = i;
        head = i;
        _flBitmap |= 1u << (fl - FlShift);
        _slBitmap[fl - FlShift] |= 1u << sl;
        return;
}

void RangeHeap::listRemove(uint32_t i) {
        const Node    &n = _nodes[i];
        const unsigned fl = msb(n.size);
        const unsigned sl = (n.size >> (fl - SlBits)) & (SlCount - 1);
        if (n.freePrev != None) {
                _nodes[n.freePrev].freeNext = n.freeNext;
        } else {
                _lists[fl - FlShift][sl] = n.freeNext;
        }
        if (n.freeNext != None) _nodes[n.freeNext].freePrev = n.freePrev;
        if (_lists[fl - FlShift][sl] == None) {
                _slBitmap[fl - FlShift] &= ~(1u << sl);
                if (_slBitmap[fl - FlShift] == 0) _flBitmap &= ~(1u << (fl - FlShift));
        }
        return;
}

// A free block of @p bytes at least: the first of the smallest class whose every block fits.
uint32_t RangeHeap::findFree(uint32_t bytes) const {
        const unsigned rawFl = msb(bytes);
        const uint32_t round = (1u << (rawFl - SlBits)) - 1;
        if (bytes > UINT32_MAX - round) return None;
        const uint32_t rounded = bytes + round;
        const unsigned fl = msb(rounded) - FlShift;
        const unsigned sl = (rounded >> (msb(rounded) - SlBits)) & (SlCount - 1);
        uint32_t       slMap = _slBitmap[fl] & (~0u << sl);
        unsigned       f = fl;
        if (slMap == 0) {
                const uint32_t flMap = _flBitmap & (~0u << (fl + 1));
                if (flMap != 0) {
                        f = unsigned(__builtin_ctz(flMap));
                        slMap = _slBitmap[f];
                }
        }
        if (slMap != 0) return _lists[f][__builtin_ctz(slMap)];
        // None that big in a class above: a block of its own class may still fit (the top of a
        // range, which would otherwise grow for nothing).
        const unsigned ownFl = rawFl - FlShift;
        const unsigned ownSl = (bytes >> (rawFl - SlBits)) & (SlCount - 1);
        for (uint32_t i = _lists[ownFl][ownSl]; i != None; i = _nodes[i].freeNext)
                if (_nodes[i].size >= bytes) return i;
        return None;
}

// ---- Blocks

// Merges free @p i with its free neighbours; returns the merged record (not in a list).
uint32_t RangeHeap::merge(uint32_t i) {
        const uint32_t p = _nodes[i].prev;
        if (p != None && _nodes[p].owner == FreeOwner) {
                listRemove(p);
                _nodes[p].size += _nodes[i].size;
                _nodes[p].next = _nodes[i].next;
                if (_nodes[i].next != None) _nodes[_nodes[i].next].prev = p;
                hashRemove(i);
                dropRecord(i);
                i = p;
        }
        const uint32_t n = _nodes[i].next;
        if (n != None && _nodes[n].owner == FreeOwner) {
                listRemove(n);
                _nodes[i].size += _nodes[n].size;
                _nodes[i].next = _nodes[n].next;
                if (_nodes[n].next != None) _nodes[_nodes[n].next].prev = i;
                hashRemove(n);
                dropRecord(n);
        }
        return i;
}

// Cuts @p i after its first @p bytes: the rest a new free record (not in a list), returned.
// A record and a hash slot must be there (reserve(), growHash()).
uint32_t RangeHeap::split(uint32_t i, uint32_t bytes) {
        const uint32_t j = takeRecord();
        Node          &a = _nodes[i];
        Node          &b = _nodes[j];
        b.at = a.at + bytes;
        b.size = a.size - bytes;
        b.prev = i;
        b.next = a.next;
        b.owner = FreeOwner;
        if (a.next != None) _nodes[a.next].prev = j;
        a.next = j;
        a.size = bytes;
        hashInsert(j);
        return j;
}

Error RangeHeap::addRange(uint32_t at, uint32_t bytes) {
        if (bytes == 0 || at % Unit != 0 || bytes % Unit != 0 || at > UINT32_MAX - bytes) return Error::Invalid;
        if (find(at) != None) return Error::Invalid;
        Error err = reserve(1);
        if (err.isOk() && _hashed + 1 > _bucketCount) err = growHash();
        if (err.isError()) return err;
        const uint32_t j = takeRecord();
        Node          &n = _nodes[j];
        n.at = at;
        n.size = bytes;
        n.prev = n.next = None;
        n.owner = FreeOwner;
        // Its neighbours, if adjacent: the block ending at its start (a walk: ranges are added
        // seldom), the one starting at its end.
        for (uint32_t i = 0; i < _capacity; ++i) {
                const Node &e = _nodes[i];
                if (i == j || e.owner == SpareOwner || e.at + e.size != at) continue;
                n.prev = i;
                _nodes[i].next = j;
                break;
        }
        const uint32_t after = at + bytes != 0 ? find(at + bytes) : None;
        if (after != None) {
                n.next = after;
                _nodes[after].prev = j;
        }
        hashInsert(j);
        _freeBytes += bytes;
        listInsert(merge(j));
        return Error::Ok;
}

Result<uint32_t> RangeHeap::alloc(uint32_t bytes, uint32_t align, uint32_t owner) {
        if (owner >= SpareOwner) return makeError<uint32_t>(Error::Invalid);
        if (align < Unit) align = Unit;
        if (!isPowerOfTwo(align) || align > MaxAlign) return makeError<uint32_t>(Error::Invalid);
        if (bytes == 0) bytes = 1;
        if (bytes > UINT32_MAX - Unit - MaxAlign) return makeError<uint32_t>(Error::NoMem);
        const uint32_t size = (bytes + Unit - 1) / Unit * Unit;
        const uint32_t need = size + (align - Unit);
        Error          err = reserve(2);
        if (err.isOk() && _hashed + 2 > _bucketCount) err = growHash();
        if (err.isError()) return makeError<uint32_t>(err);
        uint32_t i = findFree(need);
        if (i == None) return makeError<uint32_t>(Error::NoMem);
        listRemove(i);
        const uint32_t front = ((_nodes[i].at + align - 1) & ~(align - 1)) - _nodes[i].at;
        if (front != 0) {
                const uint32_t j = split(i, front);
                listInsert(i);
                i = j;
        }
        if (_nodes[i].size > size) listInsert(split(i, size));
        _nodes[i].owner = owner;
        _usedBytes += size;
        _freeBytes -= size;
        ++_blocks;
        return makeResult(_nodes[i].at);
}

RangeHeap::Freed RangeHeap::free(uint32_t at, uint32_t owner) {
        Freed          f;
        const uint32_t i = find(at);
        if (i == None || owner >= SpareOwner || _nodes[i].owner != owner) return f;
        f.bytes = _nodes[i].size;
        _nodes[i].owner = FreeOwner;
        _usedBytes -= f.bytes;
        _freeBytes += f.bytes;
        --_blocks;
        const uint32_t m = merge(i);
        listInsert(m);
        f.freeAt = _nodes[m].at;
        f.freeBytes = _nodes[m].size;
        return f;
}

void RangeHeap::freeOwner(uint32_t owner) {
        if (owner >= SpareOwner) return;
        for (uint32_t i = 0; i < _capacity; ++i)
                if (_nodes[i].owner == owner) (void)free(_nodes[i].at, owner);
        return;
}

Error RangeHeap::resize(uint32_t at, uint32_t bytes, uint32_t owner) {
        const uint32_t i = find(at);
        if (i == None || owner >= SpareOwner || _nodes[i].owner != owner) return Error::NotExist;
        if (bytes == 0) bytes = 1;
        if (bytes > UINT32_MAX - Unit) return Error::NoMem;
        const uint32_t size = (bytes + Unit - 1) / Unit * Unit;
        const uint32_t old = _nodes[i].size;
        if (size == old) return Error::Ok;
        if (size < old) {
                Error err = reserve(1);
                if (err.isOk() && _hashed + 1 > _bucketCount) err = growHash();
                if (err.isError()) return err;
                const uint32_t k = split(i, size);
                _usedBytes -= old - size;
                _freeBytes += old - size;
                listInsert(merge(k));
                return Error::Ok;
        }
        const uint32_t n = _nodes[i].next;
        const uint32_t extra = size - old;
        if (n == None || _nodes[n].owner != FreeOwner || _nodes[n].size < extra) return Error::NoMem;
        listRemove(n);
        if (_nodes[n].size > extra) {
                hashRemove(n);
                _nodes[n].at += extra;
                _nodes[n].size -= extra;
                hashInsert(n);
                listInsert(n);
        } else {
                _nodes[i].next = _nodes[n].next;
                if (_nodes[n].next != None) _nodes[_nodes[n].next].prev = i;
                hashRemove(n);
                dropRecord(n);
        }
        _nodes[i].size = size;
        _usedBytes += extra;
        _freeBytes -= extra;
        return Error::Ok;
}

uint32_t RangeHeap::sizeOf(uint32_t at, uint32_t owner) const {
        const uint32_t i = find(at);
        return i != None && owner < SpareOwner && _nodes[i].owner == owner ? _nodes[i].size : 0;
}

bool RangeHeap::contains(uint32_t at, uint32_t bytes, uint32_t owner) const {
        if (owner >= SpareOwner) return false;
        for (uint32_t i = 0; i < _capacity; ++i) {
                const Node &n = _nodes[i];
                if (n.owner == owner && at >= n.at && bytes <= n.size && at - n.at <= n.size - bytes) return true;
        }
        return false;
}

Error RangeHeap::removeEnd(uint32_t freeAt, uint32_t bytes) {
        const uint32_t i = find(freeAt);
        if (i == None || _nodes[i].owner != FreeOwner || bytes == 0 || bytes % Unit != 0 || _nodes[i].size < bytes)
                return Error::NotExist;
        listRemove(i);
        Node &n = _nodes[i];
        if (n.next != None) _nodes[n.next].prev = None; // no longer adjacent
        n.next = None;
        _freeBytes -= bytes;
        if (n.size == bytes) {
                if (n.prev != None) _nodes[n.prev].next = None;
                hashRemove(i);
                dropRecord(i);
                return Error::Ok;
        }
        n.size -= bytes;
        listInsert(i);
        return Error::Ok;
}

RangeHeap::Stats RangeHeap::stats() const {
        Stats s;
        s.usedBytes = _usedBytes;
        s.freeBytes = _freeBytes;
        s.blocks = _blocks;
        if (_flBitmap != 0) {
                const unsigned fl = msb(_flBitmap);
                const unsigned sl = msb(_slBitmap[fl]);
                for (uint32_t i = _lists[fl][sl]; i != None; i = _nodes[i].freeNext)
                        if (_nodes[i].size > s.largestFree) s.largestFree = _nodes[i].size;
        }
        return s;
}

BMDS_NAMESPACE_END
