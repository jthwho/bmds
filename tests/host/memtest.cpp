/**
 * @file      memtest.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/memtest.h>

using namespace bmds;

TEST_CASE("MemTest: every pattern fills and verifies clean") {
        uint32_t words[256];
        for (MemTest::Pattern p : MemTest::Patterns) {
                MemTest::fill(p, 7, words, 256, 0x48000000);
                CHECK(MemTest::verify(p, 7, words, 256, 0x48000000, nullptr, 0) == 0);
        }
}

TEST_CASE("MemTest: a flipped bit is reported where it is") {
        uint32_t words[64];
        MemTest::fill(MemTest::Pattern::Random, 3, words, 64, 0x1000);
        words[10] ^= 0x100;
        MemTest::Failure f[4];
        CHECK(MemTest::verify(MemTest::Pattern::Random, 3, words, 64, 0x1000, f, 4) == 1);
        CHECK(f[0].address == reinterpret_cast<uintptr_t>(&words[10]));
        CHECK((f[0].expected ^ f[0].actual) == 0x100);
        CHECK(f[0].expected == MemTest::expected(MemTest::Pattern::Random, 3, 0x1000 + 40));
}

TEST_CASE("MemTest: keys, not positions, decide the values") {
        uint32_t whole[32], part[16];
        MemTest::fill(MemTest::Pattern::Random, 1, whole, 32, 0x2000);
        MemTest::fill(MemTest::Pattern::Random, 1, part, 16, 0x2000 + 64);
        CHECK(MemTest::verify(MemTest::Pattern::Random, 1, part, 16, 0x2000 + 64, nullptr, 0) == 0);
        for (int i = 0; i < 16; ++i) CHECK(part[i] == whole[16 + i]);
}

TEST_CASE("MemTest: patterns are distinct and the seed changes the sequence") {
        CHECK(MemTest::expected(MemTest::Pattern::Address, 0, 0x40) == 0x40);
        CHECK(MemTest::expected(MemTest::Pattern::InvertedAddress, 0, 0x40) == ~uint32_t(0x40));
        CHECK(MemTest::expected(MemTest::Pattern::Checkerboard, 0, 0) !=
              MemTest::expected(MemTest::Pattern::Checkerboard, 0, 4));
        int same = 0;
        for (uintptr_t k = 0; k < 4096; k += 4)
                same += MemTest::expected(MemTest::Pattern::Random, 1, k) ==
                        MemTest::expected(MemTest::Pattern::Random, 2, k);
        CHECK(same < 4);
}
