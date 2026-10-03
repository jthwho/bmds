/**
 * @file      bytering.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/assert.h>
#include <bmds/bytering.h>

BMDS_NAMESPACE_BEGIN

ByteRing::ByteRing(WritableByteSpan storage) : _data(storage.data()), _mask(static_cast<uint32_t>(storage.size()) - 1) {
        BMDS_ASSERT(!storage.isEmpty() && (storage.size() & (storage.size() - 1)) == 0);
}

size_t ByteRing::available() const {
        // Each side loads the other side's index with acquire, so the bytes it
        // published are visible before we touch them.
        const uint32_t head = _head.load(std::memory_order_acquire);
        const uint32_t tail = _tail.load(std::memory_order_acquire);
        return head - tail;
}

size_t ByteRing::push(ByteSpan data) {
        size_t done = 0;
        while (done < data.size()) {
                WritableByteSpan dst = writable();
                if (dst.isEmpty()) break;
                const size_t n = dst.size() < data.size() - done ? dst.size() : data.size() - done;
                std::memcpy(dst.data(), data.data() + done, n);
                produce(n);
                done += n;
        }
        return done;
}

size_t ByteRing::pop(WritableByteSpan buf) {
        size_t done = 0;
        while (done < buf.size()) {
                ByteSpan src = readable();
                if (src.isEmpty()) break;
                const size_t n = src.size() < buf.size() - done ? src.size() : buf.size() - done;
                std::memcpy(buf.data() + done, src.data(), n);
                consume(n);
                done += n;
        }
        return done;
}

ByteSpan ByteRing::readable() const {
        const uint32_t tail = _tail.load(std::memory_order_relaxed);
        const uint32_t head = _head.load(std::memory_order_acquire);
        const uint32_t offset = tail & _mask;
        const uint32_t used = head - tail;
        const uint32_t toEnd = _mask + 1 - offset;
        return ByteSpan(_data + offset, used < toEnd ? used : toEnd);
}

void ByteRing::consume(size_t count) {
        const uint32_t tail = _tail.load(std::memory_order_relaxed);
        BMDS_ASSERT(count <= _head.load(std::memory_order_acquire) - tail);
        _tail.store(tail + static_cast<uint32_t>(count), std::memory_order_release);
        return;
}

WritableByteSpan ByteRing::writable() {
        const uint32_t head = _head.load(std::memory_order_relaxed);
        const uint32_t tail = _tail.load(std::memory_order_acquire);
        const uint32_t offset = head & _mask;
        const uint32_t space = _mask + 1 - (head - tail);
        const uint32_t toEnd = _mask + 1 - offset;
        return WritableByteSpan(_data + offset, space < toEnd ? space : toEnd);
}

void ByteRing::produce(size_t count) {
        const uint32_t head = _head.load(std::memory_order_relaxed);
        BMDS_ASSERT(count <= _mask + 1 - (head - _tail.load(std::memory_order_acquire)));
        _head.store(head + static_cast<uint32_t>(count), std::memory_order_release);
        return;
}

BMDS_NAMESPACE_END
