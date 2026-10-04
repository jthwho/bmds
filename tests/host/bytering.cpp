/**
 * @file      bytering.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstring>
#include <bmds/bytering.h>

using bmds::ByteRing;
using bmds::ByteSpan;
using bmds::WritableByteSpan;

namespace {

        ByteSpan bytes(const char *s) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(s), std::strlen(s));
        }

} // namespace

TEST_CASE("ByteRing") {
        uint8_t  storage[8];
        ByteRing ring{WritableByteSpan(storage)};

        SUBCASE("starts empty") {
                CHECK(ring.capacity() == 8);
                CHECK(ring.isEmpty());
                CHECK(ring.freeSpace() == 8);
                CHECK(ring.readable().isEmpty());
        }

        SUBCASE("push and pop") {
                CHECK(ring.push(bytes("abc")) == 3);
                CHECK(ring.available() == 3);
                uint8_t out[8] = {};
                CHECK(ring.pop(WritableByteSpan(out)) == 3);
                CHECK(std::memcmp(out, "abc", 3) == 0);
                CHECK(ring.isEmpty());
        }

        SUBCASE("full ring accepts nothing more") {
                CHECK(ring.push(bytes("0123456789")) == 8);
                CHECK(ring.freeSpace() == 0);
                CHECK(ring.push(bytes("x")) == 0);
                CHECK(ring.writable().isEmpty());
        }

        SUBCASE("wraps around") {
                uint8_t out[8] = {};
                CHECK(ring.push(bytes("012345")) == 6);
                CHECK(ring.pop(WritableByteSpan(out, 5)) == 5);
                CHECK(ring.push(bytes("abcdef")) == 6); // 2 at the end, 4 at the start
                CHECK(ring.available() == 7);
                CHECK(ring.readable().size() == 3); // "5ab" up to the end of storage
                CHECK(ring.pop(WritableByteSpan(out)) == 7);
                CHECK(std::memcmp(out, "5abcdef", 7) == 0);
        }

        SUBCASE("in-place produce and consume") {
                WritableByteSpan w = ring.writable();
                REQUIRE(w.size() == 8);
                std::memcpy(w.data(), "hi", 2);
                ring.produce(2);
                ByteSpan r = ring.readable();
                REQUIRE(r.size() == 2);
                CHECK(r[0] == 'h');
                ring.consume(1);
                CHECK(ring.available() == 1);
                CHECK(ring.readable()[0] == 'i');
        }

        SUBCASE("indices survive 32-bit wrap") {
                // Run the free-running indices past many laps.
                uint8_t out[3];
                for (int i = 0; i < 5000; ++i) {
                        REQUIRE(ring.push(bytes("xyz")) == 3);
                        REQUIRE(ring.pop(WritableByteSpan(out)) == 3);
                }
                CHECK(ring.isEmpty());
                CHECK(ring.freeSpace() == 8);
        }
}

TEST_CASE("ByteRing: discardTo") {
        uint8_t        storage[16];
        bmds::ByteRing ring{bmds::WritableByteSpan(storage)};
        const uint8_t  abc[] = {'a', 'b', 'c'};
        const uint8_t  de[] = {'d', 'e'};
        CHECK(ring.push(bmds::ByteSpan(abc)) == 3);
        const uint32_t mark = ring.writePosition();
        CHECK(ring.push(bmds::ByteSpan(de)) == 2);
        ring.discardTo(mark);
        uint8_t out[8];
        CHECK(ring.pop(bmds::WritableByteSpan(out)) == 2);
        CHECK(out[0] == 'd');
        ring.discardTo(mark); // already past: no effect
        CHECK(ring.isEmpty());

        // Across the wrap of the free-running indices.
        for (int i = 0; i < 100; ++i) {
                CHECK(ring.push(bmds::ByteSpan(abc)) == 3);
                const uint32_t m = ring.writePosition();
                CHECK(ring.push(bmds::ByteSpan(de)) == 2);
                ring.discardTo(m);
                CHECK(ring.available() == 2);
                CHECK(ring.pop(bmds::WritableByteSpan(out)) == 2);
        }
}
