/**
 * @file      arena.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <bmds/arena.h>
#include "hostmemory.h"

using bmds::Arena;
using bmds::Error;
using bmds::MemSpace;
using bmds::StringView;

namespace {

        struct Node {
                        Node      *left = nullptr;
                        Node      *right = nullptr;
                        StringView text;
                        int        value = 0;
        };

        bool aligned(const void *p, size_t align) {
                return reinterpret_cast<uintptr_t>(p) % align == 0;
        }

} // namespace

TEST_CASE("Arena: allocates nothing until used") {
        const size_t base = HostMemory::liveAllocations();
        Arena        arena(MemSpace::Psram, 1024);
        CHECK(arena.chunkCount() == 0);
        CHECK(arena.bytesUsed() == 0);
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Arena: bump allocation, alignment, chunking") {
        const size_t base = HostMemory::liveAllocations();
        {
                Arena arena(MemSpace::Psram, 1024);
                void *a = arena.alloc(1, 1);
                void *b = arena.alloc(1, 1);
                CHECK(static_cast<char *>(b) == static_cast<char *>(a) + 1);
                void *c = arena.alloc(8, 8);
                CHECK(aligned(c, 8));
                void *d = arena.alloc(4, 64);
                CHECK(aligned(d, 64));
                CHECK(arena.chunkCount() == 1);

                for (int i = 0; i < 100; ++i) REQUIRE(arena.alloc(100) != nullptr);
                CHECK(arena.chunkCount() > 1);
                CHECK(HostMemory::liveAllocations() == base + arena.chunkCount());
                CHECK(arena.bytesUsed() >= 10000);
                CHECK(arena.bytesReserved() >= arena.bytesUsed());

                // Bigger than a chunk: gets a chunk of its own.
                void *big = arena.alloc(5000, 16);
                CHECK(big != nullptr);
                CHECK(aligned(big, 16));
                std::memset(big, 0xa5, 5000);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Arena: create, createArray, copyString") {
        Arena arena(MemSpace::Psram, 256);
        Node *root = arena.create<Node>();
        REQUIRE(root != nullptr);
        CHECK(root->left == nullptr);
        root->left = arena.create<Node>(Node{nullptr, nullptr, "x", 7});
        CHECK(root->left->value == 7);

        int *ints = arena.createArray<int>(10);
        REQUIRE(ints != nullptr);
        for (int i = 0; i < 10; ++i) CHECK(ints[i] == 0);
        CHECK(arena.createArray<int>(0) == nullptr);

        auto [s, err] = arena.copyString(StringView("hello world", 5));
        CHECK(err.isOk());
        CHECK(s == "hello");
        CHECK(s.data()[5] == '\0');
        auto [e, err2] = arena.copyString(StringView());
        CHECK(err2.isOk());
        CHECK(e.isEmpty());
        CHECK(e.data()[0] == '\0');
}

TEST_CASE("Arena: mark and rewind") {
        const size_t base = HostMemory::liveAllocations();
        {
                Arena        arena(MemSpace::Psram, 512);
                Arena::Mark  start = arena.mark();
                void        *first = arena.alloc(16);
                Arena::Mark  m = arena.mark();
                const size_t used = arena.bytesUsed();
                for (int i = 0; i < 50; ++i) REQUIRE(arena.alloc(64) != nullptr);
                CHECK(arena.chunkCount() > 1);
                arena.rewind(m);
                CHECK(arena.chunkCount() == 1);
                CHECK(arena.bytesUsed() == used);
                // The next allocation reuses the released space.
                void *again = arena.alloc(16);
                CHECK(static_cast<char *>(again) == static_cast<char *>(first) + 16);

                arena.rewind(start); // before anything: same as reset()
                CHECK(arena.chunkCount() == 1);
                CHECK(arena.bytesUsed() == 0);
                CHECK(arena.alloc(16) == first);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Arena: reset keeps one regular chunk; release frees all") {
        const size_t base = HostMemory::liveAllocations();
        Arena        arena(MemSpace::Psram, 512);
        for (int i = 0; i < 20; ++i) REQUIRE(arena.alloc(100) != nullptr);
        arena.reset();
        CHECK(arena.chunkCount() == 1);
        CHECK(arena.bytesUsed() == 0);
        CHECK(HostMemory::liveAllocations() == base + 1);
        arena.release();
        CHECK(arena.chunkCount() == 0);
        CHECK(HostMemory::liveAllocations() == base);

        // An oversized first chunk isn't kept.
        REQUIRE(arena.alloc(4096) != nullptr);
        arena.reset();
        CHECK(arena.chunkCount() == 0);
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Arena: allocation failure returns null") {
        const size_t base = HostMemory::liveAllocations();
        {
                Arena arena(MemSpace::Psram, 256);
                {
                        HostMemory::FailGuard fail;
                        CHECK(arena.alloc(8) == nullptr);
                        CHECK(arena.create<Node>() == nullptr);
                        auto [s, err] = arena.copyString("x");
                        CHECK(err == Error::NoMem);
                        CHECK(arena.chunkCount() == 0);
                }
                REQUIRE(arena.alloc(8) != nullptr);
                {
                        HostMemory::FailGuard fail;
                        CHECK(arena.alloc(8) != nullptr); // still room in the chunk
                        CHECK(arena.alloc(1000) == nullptr);
                        CHECK(arena.chunkCount() == 1);
                }
                CHECK(arena.alloc(static_cast<size_t>(-1) - 8) == nullptr); // size overflow
        }
        CHECK(HostMemory::liveAllocations() == base);
}
