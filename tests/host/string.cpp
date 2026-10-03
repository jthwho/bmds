/**
 * @file      string.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <string>
#include <bmds/string.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::MemSpace;
using bmds::String;
using bmds::StringView;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        // Builds a string that needs a heap block.
        String longString(const char *text = "0123456789abcdefghijklmnopqrstuvwxyz") {
                auto [s, err] = String::fromView(text);
                REQUIRE(err.isOk());
                return std::move(s);
        }

} // namespace

TEST_CASE("String: empty and inline") {
        const size_t base = HostMemory::liveAllocations();
        {
                String s;
                CHECK(s.isEmpty());
                CHECK(s.size() == 0);
                CHECK(s.cStr()[0] == '\0');
                CHECK(s.space() == String::DefaultSpace);
                CHECK(s.capacity() == String::InlineCapacity);

                String t(MemSpace::Internal);
                CHECK(t.space() == MemSpace::Internal);

                // Short text never allocates.
                CHECK(s.append("hello").isOk());
                CHECK(s.append(' ').isOk());
                CHECK(s.append("world!!!!").isOk());
                CHECK(s.size() == String::InlineCapacity);
                CHECK(HostMemory::liveAllocations() == base);
                CHECK(s == "hello world!!!!");
                CHECK(s.cStr()[s.size()] == '\0');
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("String: growth to the heap and back") {
        const size_t base = HostMemory::liveAllocations();
        {
                String s(MemSpace::Psram);
                for (int i = 0; i < 100; ++i) REQUIRE(s.append(static_cast<char>('a' + i % 26)).isOk());
                CHECK(s.size() == 100);
                CHECK(s.capacity() >= 100);
                CHECK(HostMemory::liveAllocations() == base + 1);
                CHECK(s[0] == 'a');
                CHECK(s[26] == 'a');
                CHECK(s.cStr()[100] == '\0');
                s.clear();
                CHECK(s.isEmpty());
                CHECK(s.capacity() == String::InlineCapacity);
                CHECK(HostMemory::liveAllocations() == base);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("String: fromView, format, clone") {
        auto [a, errA] = String::fromView("abc", MemSpace::Internal);
        CHECK(errA.isOk());
        CHECK(a == "abc");
        CHECK(a.space() == MemSpace::Internal);

        auto [f, errF] = String::format(MemSpace::Psram, "%d-%s-%04x", 42, "x", 0xbeef);
        CHECK(errF.isOk());
        CHECK(f == "42-x-beef");

        String big = longString();
        auto [c, errC] = big.clone();
        CHECK(errC.isOk());
        CHECK(c == big);
        CHECK(c.data() != big.data());
        CHECK(c.space() == big.space());
}

TEST_CASE("String: move") {
        const size_t base = HostMemory::liveAllocations();
        {
                String      a = longString();
                const char *heap = a.data();
                String      b(std::move(a));
                CHECK(b.data() == heap);
                CHECK(a.isEmpty());
                CHECK(a.cStr()[0] == '\0');

                auto [small, err] = String::fromView("tiny");
                REQUIRE(err.isOk());
                b = std::move(small);
                CHECK(b == "tiny");
                CHECK(small.isEmpty());
                CHECK(HostMemory::liveAllocations() == base);

                String  c = longString();
                String &alias = c;
                c = std::move(alias);
                CHECK(c.size() == 36);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("String: assign, insert, remove, truncate") {
        String s;
        CHECK(s.assign("hello world").isOk());
        CHECK(s.insert(5, ",").isOk());
        CHECK(s == "hello, world");
        CHECK(s.insert(0, ">> ").isOk());
        CHECK(s.insert(999, "!").isOk());
        CHECK(s == ">> hello, world!");
        s.remove(0, 3);
        CHECK(s == "hello, world!");
        s.remove(5, 1);
        CHECK(s == "hello world!");
        s.remove(5);
        CHECK(s == "hello");
        s.remove(99, 1);
        CHECK(s == "hello");
        s.truncate(2);
        CHECK(s == "he");
        CHECK(s.cStr()[2] == '\0');
        s.truncate(10);
        CHECK(s == "he");
        CHECK(s.assign("").isOk());
        CHECK(s.isEmpty());
        CHECK(s.append(StringView()).isOk());
        CHECK(s.insert(0, StringView()).isOk());
        CHECK(s.isEmpty());
}

TEST_CASE("String: operations on views of itself") {
        String s = longString("0123456789");
        CHECK(s.append(s.view()).isOk()); // grows to the heap while reading itself
        CHECK(s == "01234567890123456789");
        CHECK(s.append(s.view()).isOk());
        CHECK(s.size() == 40);
        CHECK(s.assign(s.view().mid(10, 5)).isOk());
        CHECK(s == "01234");
        CHECK(s.insert(2, s.view().left(3)).isOk());
        CHECK(s == "01012234");
}

TEST_CASE("String: appendf grows when the text doesn't fit") {
        String s;
        CHECK(s.appendf("%s", "short").isOk());
        CHECK(s.appendf(" %s %d", "a much longer piece of formatted text", 12345).isOk());
        CHECK(str(s) == "short a much longer piece of formatted text 12345");
        CHECK(s.cStr()[s.size()] == '\0');
}

TEST_CASE("String: comparisons") {
        auto [a, e1] = String::fromView("apple");
        auto [b, e2] = String::fromView("banana");
        CHECK(a < b);
        CHECK(b > a);
        CHECK(a == StringView("apple"));
        CHECK(a != b);
        CHECK(std::is_eq(a <=> StringView("apple")));
        StringView v = a; // implicit conversion
        CHECK(v.endsWith("ple"));
}

TEST_CASE("String: allocation failure leaves the string unchanged") {
        const size_t base = HostMemory::liveAllocations();
        {
                auto [s, err] = String::fromView("abc");
                REQUIRE(err.isOk());
                {
                        HostMemory::FailGuard fail;
                        CHECK(s.append("this text does not fit inline") == Error::NoMem);
                        CHECK(s == "abc");
                        CHECK(s.assign("this text does not fit inline") == Error::NoMem);
                        CHECK(s == "abc");
                        CHECK(s.insert(1, "this text does not fit inline") == Error::NoMem);
                        CHECK(s == "abc");
                        CHECK(s.appendf("%s", "this text does not fit inline") == Error::NoMem);
                        CHECK(s == "abc");
                        CHECK(s.reserve(100) == Error::NoMem);
                        CHECK(s.capacity() == String::InlineCapacity);
                        // Inline operations still work.
                        CHECK(s.append("def").isOk());
                        CHECK(s == "abcdef");

                        auto [c, errC] = String::fromView("this text does not fit inline");
                        CHECK(errC == Error::NoMem);
                        CHECK(c.isEmpty());
                }
                String big = longString();
                {
                        HostMemory::FailGuard fail;
                        auto [c, errC] = big.clone();
                        CHECK(errC == Error::NoMem);
                        CHECK(big.append(big.view()) == Error::NoMem);
                        CHECK(big.size() == 36);
                        CHECK(big.insert(0, big.view()) == Error::NoMem);
                        CHECK(big.size() == 36);
                }
        }
        CHECK(HostMemory::liveAllocations() == base);
}
