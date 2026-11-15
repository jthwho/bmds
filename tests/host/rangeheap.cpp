/**
 * @file      rangeheap.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A U-mode program's heap with its records outside it (devplan/protection.md §15): sizes and
 * alignment, merging, owners, growth in place, ranges added and given back, running out of
 * records, and a random soak against a model.
 */

#include <doctest.h>

#include <cstdint>
#include <map>
#include <random>
#include <bmds/rangeheap.h>

#include "hostmemory.h"

using bmds::Error;
using bmds::RangeHeap;

namespace {
        constexpr uint32_t Base = 0x4a000000u;
        constexpr uint32_t Me = 1;
        constexpr uint32_t Child = 2;
} // namespace

TEST_CASE("RangeHeap: blocks are Unit multiples, merged when freed") {
        RangeHeap h;
        CHECK(h.alloc(16, 0, Me).error() == Error::NoMem); // no range yet
        REQUIRE(h.addRange(Base, 4096) == Error::Ok);
        auto a = h.alloc(1, 0, Me);
        auto b = h.alloc(17, 0, Me);
        auto c = h.alloc(100, 0, Me);
        REQUIRE(a.isOk());
        REQUIRE(b.isOk());
        REQUIRE(c.isOk());
        CHECK(a.value() % RangeHeap::Unit == 0);
        CHECK(h.sizeOf(a.value(), Me) == 16);
        CHECK(h.sizeOf(b.value(), Me) == 32);
        CHECK(h.sizeOf(c.value(), Me) == 112);
        CHECK(h.stats().usedBytes == 160);
        CHECK(h.stats().freeBytes == 4096 - 160);
        CHECK(h.stats().blocks == 3);
        // Not a block's start, or another owner's: nothing.
        CHECK(h.free(c.value() + 16, Me).bytes == 0);
        CHECK(h.free(a.value(), Child).bytes == 0);
        CHECK(h.free(b.value(), Me).bytes == 32);
        CHECK(h.free(b.value(), Me).bytes == 0); // twice
        (void)h.free(a.value(), Me);
        const RangeHeap::Freed f = h.free(c.value(), Me);
        CHECK(f.bytes == 112);
        CHECK(f.freeAt == Base);
        CHECK(f.freeBytes == 4096); // all merged again
        CHECK(h.stats().blocks == 0);
        CHECK(h.stats().largestFree == 4096);
        // The whole range fits again.
        CHECK(h.alloc(4096, 0, Me).isOk());
        CHECK(h.alloc(16, 0, Me).error() == Error::NoMem);
}

TEST_CASE("RangeHeap: alignment") {
        RangeHeap h;
        REQUIRE(h.addRange(Base + 16, 64 * 1024 - 16) == Error::Ok);
        for (uint32_t align : {16u, 32u, 128u, 4096u}) {
                auto r = h.alloc(48, align, Me);
                REQUIRE(r.isOk());
                CHECK(r.value() % align == 0);
        }
        CHECK(h.alloc(16, 24, Me).error() == Error::Invalid);
        CHECK(h.alloc(16, RangeHeap::MaxAlign * 2, Me).error() == Error::Invalid);
        CHECK(h.alloc(16, 0, UINT32_MAX).error() == Error::Invalid);
        // The space cut off in front stays usable.
        CHECK(h.alloc(16, 0, Me).value() < Base + 4096);
}

TEST_CASE("RangeHeap: owners") {
        RangeHeap h;
        REQUIRE(h.addRange(Base, 8192) == Error::Ok);
        auto mine = h.alloc(256, 0, Me).value();
        auto theirs1 = h.alloc(256, 0, Child).value();
        auto theirs2 = h.alloc(512, 0, Child).value();
        auto kernel = h.alloc(64, 0, 0).value();
        CHECK(h.contains(mine + 10, 20, Me));
        CHECK_FALSE(h.contains(mine + 250, 20, Me)); // past its end
        CHECK_FALSE(h.contains(theirs1, 16, Me));
        CHECK(h.contains(kernel, 64, 0));
        h.freeOwner(Child);
        CHECK(h.sizeOf(theirs1, Child) == 0);
        CHECK(h.sizeOf(theirs2, Child) == 0);
        CHECK(h.sizeOf(mine, Me) == 256);
        CHECK(h.sizeOf(kernel, 0) == 64);
        CHECK(h.stats().blocks == 2);
        CHECK(h.stats().usedBytes == 320);
}

TEST_CASE("RangeHeap: resizing in place") {
        RangeHeap h;
        REQUIRE(h.addRange(Base, 4096) == Error::Ok);
        auto a = h.alloc(64, 0, Me).value();
        auto b = h.alloc(64, 0, Me).value();
        CHECK(h.resize(a, 100, Me) == Error::NoMem); // b is in the way
        CHECK(h.resize(b, 1000, Me) == Error::Ok);
        CHECK(h.sizeOf(b, Me) == 1008);
        CHECK(h.resize(b, 4096 - 64, Me) == Error::Ok); // all the rest
        CHECK(h.stats().freeBytes == 0);
        CHECK(h.resize(b, 32, Me) == Error::Ok); // shrinking gives the rest back
        CHECK(h.stats().freeBytes == 4096 - 96);
        CHECK(h.alloc(4096 - 96, 0, Me).isOk());
        CHECK(h.resize(b + 16, 32, Me) == Error::NotExist);
        CHECK(h.resize(a, 64, Child) == Error::NotExist);
}

TEST_CASE("RangeHeap: ranges added on top merge; the end given back") {
        RangeHeap h;
        REQUIRE(h.addRange(Base, 4096) == Error::Ok);
        auto a = h.alloc(4000, 0, Me).value();
        REQUIRE(h.addRange(Base + 4096, 4096) == Error::Ok); // grown: merged with the 96 free
        CHECK(h.stats().largestFree == 4096 + 96);
        auto b = h.alloc(4096 + 96, 0, Me);
        REQUIRE(b.isOk());
        CHECK(b.value() == Base + 4000);
        // A range apart: not adjacent, never merged.
        REQUIRE(h.addRange(Base + 65536, 4096) == Error::Ok);
        CHECK(h.addRange(Base + 65536, 16) == Error::Invalid); // already there
        CHECK(h.addRange(Base + 3, 16) == Error::Invalid);
        CHECK(h.alloc(8192, 0, Me).error() == Error::NoMem);
        // Freed at the top: the last pages given back.
        const RangeHeap::Freed f = h.free(b.value(), Me);
        CHECK(f.freeAt == Base + 4000);
        CHECK(f.freeBytes == 4096 + 96);
        REQUIRE(h.removeEnd(f.freeAt, 4096) == Error::Ok);
        CHECK(h.stats().freeBytes == 96 + 4096);
        CHECK(h.removeEnd(f.freeAt, 4096) == Error::NotExist); // only 96 left there
        // Grown again where it ended.
        REQUIRE(h.addRange(Base + 4096, 4096) == Error::Ok);
        CHECK(h.alloc(4096 + 96, 0, Me).value() == Base + 4000);
        // The whole of a free block given back.
        auto                   c = h.alloc(4096, 0, Me).value();
        const RangeHeap::Freed g = h.free(c, Me);
        CHECK(h.removeEnd(g.freeAt, g.freeBytes) == Error::Ok);
        CHECK(h.stats().freeBytes == 0);
        (void)a;
}

TEST_CASE("RangeHeap: no memory for records changes nothing") {
        RangeHeap h;
        {
                HostMemory::FailGuard fail;
                CHECK(h.addRange(Base, 4096) == Error::NoMem);
        }
        REQUIRE(h.addRange(Base, 1 << 20) == Error::Ok);
        // Use up the records there are, then fail the next growth.
        uint32_t made = 0;
        for (;; ++made) {
                HostMemory::FailGuard fail;
                auto                  r = h.alloc(16, 0, Me);
                if (r.isError()) {
                        CHECK(r.error() == Error::NoMem);
                        break;
                }
        }
        CHECK(made > 0);
        CHECK(h.stats().blocks == made);
        CHECK(h.stats().usedBytes == made * 16);
        CHECK(h.alloc(16, 0, Me).isOk()); // with memory again
}

TEST_CASE("RangeHeap: random soak against a model") {
        std::mt19937                 rng(12345);
        RangeHeap                    h;
        std::map<uint32_t, uint32_t> live; // start -> size
        uint32_t                     top = Base;
        auto                         grow = [&](uint32_t bytes) {
                REQUIRE(h.addRange(top, bytes) == Error::Ok);
                top += bytes;
        };
        grow(64 * 1024);
        for (int step = 0; step < 20000; ++step) {
                const unsigned op = rng() % 10;
                if (op < 5 || live.empty()) {
                        const uint32_t bytes = (rng() % 4 == 0) ? 1 + rng() % 20000 : 1 + rng() % 200;
                        const uint32_t align = 1u << (4 + rng() % 9);
                        auto           r = h.alloc(bytes, align, Me);
                        if (r.isError()) {
                                grow(64 * 1024);
                                continue;
                        }
                        const uint32_t at = r.value();
                        const uint32_t size = h.sizeOf(at, Me);
                        CHECK(at % align == 0);
                        CHECK(size >= bytes);
                        CHECK(at + size <= top);
                        auto next = live.lower_bound(at);
                        if (next != live.end()) CHECK(at + size <= next->first);
                        if (next != live.begin()) {
                                auto prev = std::prev(next);
                                CHECK(prev->first + prev->second <= at);
                        }
                        live[at] = size;
                } else if (op < 8) {
                        auto it = live.begin();
                        std::advance(it, rng() % live.size());
                        CHECK(h.free(it->first, Me).bytes == it->second);
                        live.erase(it);
                } else {
                        auto it = live.begin();
                        std::advance(it, rng() % live.size());
                        const uint32_t want = 1 + rng() % 4000;
                        if (h.resize(it->first, want, Me) == Error::Ok) {
                                it->second = h.sizeOf(it->first, Me);
                                CHECK(it->second >= want);
                                auto next = std::next(it);
                                if (next != live.end()) CHECK(it->first + it->second <= next->first);
                        }
                }
                if (step % 1000 == 0) {
                        uint32_t used = 0;
                        for (auto &[at, size] : live) used += size;
                        const RangeHeap::Stats s = h.stats();
                        CHECK(s.usedBytes == used);
                        CHECK(s.blocks == live.size());
                        CHECK(s.usedBytes + s.freeBytes == top - Base);
                }
        }
        for (auto &[at, size] : live) CHECK(h.free(at, Me).bytes == size);
        const RangeHeap::Stats s = h.stats();
        CHECK(s.usedBytes == 0);
        CHECK(s.freeBytes == top - Base);
        CHECK(s.largestFree == top - Base); // all one block again
}
