/**
 * @file      list.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <bmds/list.h>
#include <bmds/string.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::List;
using bmds::MemSpace;
using bmds::Span;
using bmds::String;
using bmds::StringView;

namespace {

        // Counts live instances, to check construction/destruction balance.
        struct Tracked {
                        static inline int live = 0;
                        int               value;
                        Tracked(int v) : value(v) { ++live; }
                        Tracked(const Tracked &o) : value(o.value) { ++live; }
                        Tracked(Tracked &&o) : value(o.value) {
                                o.value = -1;
                                ++live;
                        }
                        Tracked &operator=(const Tracked &) = default;
                        Tracked &operator=(Tracked &&o) {
                                value = o.value;
                                o.value = -1;
                                return *this;
                        }
                        ~Tracked() { --live; }
                        bool operator==(const Tracked &o) const { return value == o.value; }
        };

        struct alignas(32) Wide {
                        uint8_t bytes[32];
        };

        String makeString(const char *text) {
                auto [s, err] = String::fromView(text);
                REQUIRE(err.isOk());
                return std::move(s);
        }

} // namespace

TEST_CASE("List: empty list owns nothing") {
        const size_t base = HostMemory::liveAllocations();
        List<int>    list;
        CHECK(list.isEmpty());
        CHECK(list.data() == nullptr);
        CHECK(list.capacity() == 0);
        CHECK(list.space() == List<int>::DefaultSpace);
        CHECK(List<int>(MemSpace::Internal).space() == MemSpace::Internal);
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("List: append, index, iterate, span") {
        List<int> list;
        for (int i = 0; i < 100; ++i) REQUIRE(list.append(i).isOk());
        CHECK(list.size() == 100);
        CHECK(list.front() == 0);
        CHECK(list.back() == 99);
        CHECK(list[42] == 42);
        int sum = 0;
        for (int v : list) sum += v;
        CHECK(sum == 4950);
        Span<const int> span = list; // contiguous-container conversion
        CHECK(span.size() == 100);
        CHECK(list.span()[7] == 7);
        CHECK(list.indexOf(57) == 57);
        CHECK(list.indexOf(1000) == List<int>::NPos);
        CHECK(list.contains(3));
}

TEST_CASE("List: insert, remove, take, truncate") {
        List<int> list;
        CHECK(list.append(1).isOk());
        CHECK(list.append(3).isOk());
        CHECK(list.insert(1, 2).isOk());
        CHECK(list.insert(0, 0).isOk());
        CHECK(list.insert(99, 4).isOk());
        REQUIRE(list.size() == 5);
        for (int i = 0; i < 5; ++i) CHECK(list[static_cast<size_t>(i)] == i);
        list.removeAt(0);
        list.removeAt(99);
        CHECK(list.front() == 1);
        CHECK(list.takeLast() == 4);
        CHECK(list.size() == 3);
        const size_t cap = list.capacity();
        list.truncate(1);
        CHECK(list.size() == 1);
        CHECK(list.capacity() == cap);
        list.clear();
        CHECK(list.data() == nullptr);
}

TEST_CASE("List: appending a span copies it whole, growing once") {
        List<uint8_t> list;
        CHECK(list.append(Span<const uint8_t>()).isOk());
        CHECK(list.isEmpty());
        uint8_t bytes[300];
        for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(i);
        CHECK(list.append(uint8_t(7)).isOk());
        CHECK(list.append(Span<const uint8_t>(bytes, sizeof(bytes))).isOk());
        REQUIRE(list.size() == 301);
        CHECK(list[0] == 7);
        for (size_t i = 0; i < sizeof(bytes); ++i) CHECK(list[i + 1] == bytes[i]);
        CHECK(list.capacity() >= 301);
        CHECK(list.append(Span<const uint8_t>(bytes, 4)).isOk()); // after the end
        CHECK(list.size() == 305);
        CHECK(list[304] == 3);
        list.truncate(2); // trivially destroyed: the size alone
        CHECK(list.size() == 2);
        CHECK(list[1] == 0);
        List<uint8_t> fresh; // a growth that fails leaves it as it was
        {
                HostMemory::FailGuard fail;
                CHECK(fresh.append(Span<const uint8_t>(bytes, sizeof(bytes))) == Error::NoMem);
        }
        CHECK(fresh.isEmpty());
}

TEST_CASE("List: non-trivial elements are constructed and destroyed exactly once") {
        const size_t base = HostMemory::liveAllocations();
        {
                List<Tracked> list;
                for (int i = 0; i < 50; ++i) REQUIRE(list.append(Tracked(i)).isOk());
                CHECK(Tracked::live == 50);
                CHECK(list.insert(10, Tracked(100)).isOk());
                CHECK(list[10].value == 100);
                CHECK(list[11].value == 10);
                list.removeAt(10);
                CHECK(Tracked::live == 50);
                CHECK(list[10].value == 10);

                // Appending one of its own elements, at the point it must grow.
                while (list.size() < list.capacity()) REQUIRE(list.append(Tracked(0)).isOk());
                CHECK(list.append(list[3]).isOk());
                CHECK(list.back().value == 3);
                CHECK(list.emplace(0, list.back()).isOk());
                CHECK(list.front().value == 3);

                auto [copy, err] = list.clone();
                CHECK(err.isOk());
                CHECK(copy.size() == list.size());
                CHECK(copy[5] == list[5]);

                Tracked t = list.takeLast();
                CHECK(t.value == 3);
                list.truncate(0);
                CHECK(Tracked::live == static_cast<int>(copy.size()) + 1);
        }
        CHECK(Tracked::live == 0);
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("List: move-only elements") {
        List<String> list;
        CHECK(list.append(makeString("one")).isOk());
        CHECK(list.append(makeString("a string long enough to need a heap block")).isOk());
        for (int i = 0; i < 20; ++i) REQUIRE(list.append(makeString("more")).isOk());
        CHECK(list[0] == "one");
        CHECK(list[1].size() == 41);
        String last = list.takeLast();
        CHECK(last == "more");

        List<String> moved(std::move(list));
        CHECK(list.isEmpty());
        CHECK(moved.size() == 21);
        list = std::move(moved);
        CHECK(list.size() == 21);
        CHECK(list[1].view().startsWith("a string"));
}

TEST_CASE("List: over-aligned elements") {
        List<Wide> list;
        for (int i = 0; i < 10; ++i) REQUIRE(list.append(Wide{}).isOk());
        CHECK(reinterpret_cast<uintptr_t>(list.data()) % 32 == 0);
}

TEST_CASE("List: allocation failure leaves the list unchanged") {
        const size_t base = HostMemory::liveAllocations();
        {
                List<Tracked> list;
                CHECK(list.reserve(2).isOk());
                CHECK(list.append(Tracked(1)).isOk());
                CHECK(list.append(Tracked(2)).isOk());
                {
                        HostMemory::FailGuard fail;
                        Tracked               t(3);
                        CHECK(list.append(std::move(t)) == Error::NoMem);
                        CHECK(t.value == 3); // not moved from
                        CHECK(list.insert(0, Tracked(0)) == Error::NoMem);
                        CHECK(list.reserve(100) == Error::NoMem);
                        CHECK(list.size() == 2);
                        CHECK(list[0].value == 1);
                        CHECK(list[1].value == 2);
                        auto [copy, err] = list.clone();
                        CHECK(err == Error::NoMem);
                        CHECK(copy.isEmpty());
                }
                CHECK(Tracked::live == 2);
        }
        CHECK(Tracked::live == 0);
        CHECK(HostMemory::liveAllocations() == base);
}
