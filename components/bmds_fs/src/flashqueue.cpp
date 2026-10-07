/**
 * @file      flashqueue.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/flashqueue.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A record: the address, the length, the kind, three bytes of padding, then the data.
        constexpr size_t  KindOffset = 8;
        constexpr uint8_t GapKind = 2; // the rest of the ring is unused; the next record is at 0

        uint32_t load32(const uint8_t *p) {
                uint32_t v;
                std::memcpy(&v, p, sizeof(v));
                return v;
        }

        void store32(uint8_t *p, uint32_t v) {
                std::memcpy(p, &v, sizeof(v));
                return;
        }

} // namespace

FlashQueue::~FlashQueue() {
        Memory::free(_ring);
}

Error FlashQueue::init(size_t bytes, MemSpace space) {
        if (_ring != nullptr) return Error::InvalidState;
        if (bytes < 4096 || bytes % 4 != 0) return Error::Invalid;
        _ring = static_cast<uint8_t *>(Memory::alloc(space, bytes));
        if (_ring == nullptr) return Error::NoMem;
        _capacity = bytes;
        return Error::Ok;
}

size_t FlashQueue::skipGap(size_t at) const {
        if (_capacity - at < HeaderBytes || _ring[at + KindOffset] == GapKind) return 0;
        return at;
}

Error FlashQueue::append(Kind kind, uint32_t address, uint32_t length, const uint8_t *data) {
        const size_t need = recordBytes(kind == Kind::Program ? length : 0);
        if (_count == 0) _head = _tail = _used = 0;
        // A record that doesn't fit before the end starts again at the front.
        const size_t gap = _tail + need > _capacity ? _capacity - _tail : 0;
        if (_used + gap + need > _capacity) return Error::Full;
        if (gap != 0) {
                if (gap >= HeaderBytes) _ring[_tail + KindOffset] = GapKind;
                _used += gap;
                _tail = 0;
        }
        uint8_t *record = _ring + _tail;
        store32(record, address);
        store32(record + 4, length);
        record[KindOffset] = static_cast<uint8_t>(kind);
        if (kind == Kind::Program) std::memcpy(record + HeaderBytes, data, length);
        _tail += need;
        if (_tail == _capacity) _tail = 0;
        _used += need;
        ++_count;
        return Error::Ok;
}

Error FlashQueue::tryErase(uint32_t address, uint32_t length) {
        return append(Kind::Erase, address, length, nullptr);
}

Error FlashQueue::tryProgram(uint32_t address, ByteSpan data) {
        if (data.isEmpty() || data.size() > maxProgram()) return Error::Invalid;
        return append(Kind::Program, address, static_cast<uint32_t>(data.size()), data.data());
}

void FlashQueue::overlay(uint32_t address, WritableByteSpan buf) const {
        const uint64_t start = address, end = start + buf.size();
        size_t         at = _head;
        for (size_t i = 0; i < _count; ++i) {
                at = skipGap(at);
                const uint8_t *record = _ring + at;
                const uint32_t opAddress = load32(record);
                const uint32_t length = load32(record + 4);
                const Kind     kind = static_cast<Kind>(record[KindOffset]);
                const uint64_t from = opAddress > start ? opAddress : start;
                const uint64_t to = uint64_t(opAddress) + length < end ? uint64_t(opAddress) + length : end;
                if (from < to) {
                        uint8_t *out = buf.data() + (from - start);
                        if (kind == Kind::Erase) {
                                std::memset(out, 0xff, size_t(to - from));
                        } else {
                                const uint8_t *in = record + HeaderBytes + (from - opAddress);
                                for (size_t k = 0; k < size_t(to - from); ++k) out[k] &= in[k];
                        }
                }
                at += recordBytes(kind == Kind::Program ? length : 0);
                if (at == _capacity) at = 0;
        }
        return;
}

bool FlashQueue::front(Op &op) const {
        if (_count == 0) return false;
        const uint8_t *record = _ring + skipGap(_head);
        op.address = load32(record);
        op.length = load32(record + 4);
        op.kind = static_cast<Kind>(record[KindOffset]);
        op.data = op.kind == Kind::Program ? record + HeaderBytes : nullptr;
        return true;
}

void FlashQueue::pop() {
        if (_count == 0) return;
        const size_t at = skipGap(_head);
        if (at != _head) {
                _used -= _capacity - _head;
                _head = 0;
        }
        const uint8_t *record = _ring + _head;
        const size_t   bytes =
                recordBytes(static_cast<Kind>(record[KindOffset]) == Kind::Program ? load32(record + 4) : 0);
        _head += bytes;
        if (_head == _capacity) _head = 0;
        _used -= bytes;
        if (--_count == 0) _head = _tail = _used = 0;
        return;
}

BMDS_NAMESPACE_END
