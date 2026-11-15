/**
 * @file      pmpset.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The PMP entries a U-mode process gets (devplan/protection.md §4.1, §14): NAPOT and TOR
 * encoding, the granule rule, what fits in 16, and the matching model.
 */

#include <doctest.h>

#include <bmds/pmpset.h>

using bmds::Error;
using bmds::PmpSet;

namespace {
        constexpr uint8_t Rx = PmpSet::Read | PmpSet::Exec;
        constexpr uint8_t Rw = PmpSet::Read | PmpSet::Write;
        constexpr uint8_t Rwx = Rx | PmpSet::Write;
} // namespace

TEST_CASE("PmpSet: NAPOT for an aligned power of two") {
        PmpSet s;
        // The stub page: 4 KB of flash text.
        REQUIRE(s.add(0x40123000u, 4096, Rx) == Error::Ok);
        CHECK(s.used == 1);
        CHECK(s.addr[0] == ((0x40123000u | 0x7ffu) >> 2));
        CHECK(s.config(0) == (Rx | (PmpSet::Napot << 3)));
        // PSRAM: 64 MB at 0x48000000.
        REQUIRE(s.add(0x48000000u, 64u << 20, Rwx) == Error::Ok);
        CHECK(s.used == 2);
        CHECK(s.addr[1] == ((0x48000000u | ((32u << 20) - 1)) >> 2));
        CHECK(s.cfg[0] == (uint32_t(Rx | 0x18) | (uint32_t(Rwx | 0x18) << 8)));

        CHECK(s.permits(0x40123000u, 4, PmpSet::Exec));
        CHECK(s.permits(0x40123ffcu, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x40123ffcu, 4, PmpSet::Write));
        CHECK_FALSE(s.permits(0x40124000u, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x40122ffcu, 4, PmpSet::Read));
        CHECK(s.permits(0x48000000u, 4, Rwx));
        CHECK(s.permits(0x4bfffffcu, 4, PmpSet::Write));
        CHECK_FALSE(s.permits(0x4c000000u, 4, PmpSet::Read));
        // Internal RAM and the peripherals: no entry, so denied.
        CHECK_FALSE(s.permits(0x4ff40000u, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x500a5000u, 4, PmpSet::Read));
}

TEST_CASE("PmpSet: TOR pairs, and a shared bottom") {
        PmpSet s;
        REQUIRE(s.add(0x4ff40000u, 0x3000, Rw) == Error::Ok); // 12 KB: not a power of two
        CHECK(s.used == 2);
        CHECK(s.config(0) == 0); // the bottom: off
        CHECK(s.addr[0] == 0x4ff40000u >> 2);
        CHECK(s.config(1) == (Rw | (PmpSet::Tor << 3)));
        CHECK(s.addr[1] == 0x4ff43000u >> 2);
        // Starting where it ended: one more entry.
        CHECK(s.entriesFor(0x4ff43000u, 0x180) == 1);
        REQUIRE(s.add(0x4ff43000u, 0x180, Rw) == Error::Ok);
        CHECK(s.used == 3);
        // Elsewhere: two.
        CHECK(s.entriesFor(0x4ff50080u, 0x100) == 2);

        CHECK(s.permits(0x4ff40000u, 4, Rw));
        CHECK(s.permits(0x4ff42ffcu, 4, PmpSet::Write));
        CHECK(s.permits(0x4ff4317cu, 4, PmpSet::Write));
        CHECK_FALSE(s.permits(0x4ff43180u, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x4ff3fffcu, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x4ff40000u, 4, PmpSet::Exec));
}

TEST_CASE("PmpSet: an access straddling a region's edge is denied") {
        PmpSet s;
        REQUIRE(s.add(0x4ff40000u, 0x100, Rw) == Error::Ok);
        REQUIRE(s.add(0x4ff40100u, 0x100, Rw) == Error::Ok); // NAPOT: its own entry
        CHECK(s.permits(0x4ff400fcu, 4, PmpSet::Read));
        // Partly in entry 0: entry 0 decides, and it doesn't hold all of it.
        CHECK_FALSE(s.permits(0x4ff400fe, 4, PmpSet::Read));
}

TEST_CASE("PmpSet: only whole granules") {
        PmpSet s;
        CHECK(s.add(0x4ff40010u, 0x100, Rw) == Error::Invalid);
        CHECK(s.add(0x4ff40000u, 0x110, Rw) == Error::Invalid);
        CHECK(s.add(0x4ff40000u, 0, Rw) == Error::Invalid);
        CHECK(s.add(0xffffff80u, 0x100, Rw) == Error::Invalid); // wraps
        CHECK(s.used == 0);
        // Up to 4 GB exactly is fine.
        CHECK(s.add(0xffffff00u, 0x100, PmpSet::Read) == Error::Ok);
        CHECK(s.permits(0xfffffffcu, 4, PmpSet::Read));
}

TEST_CASE("PmpSet: what fits in 16") {
        PmpSet s;
        REQUIRE(s.add(0x40123000u, 4096, Rx) == Error::Ok);
        REQUIRE(s.add(0x48000000u, 64u << 20, Rwx) == Error::Ok);
        // Seven separate TOR regions take the other 14.
        for (uint32_t i = 0; i < 7; ++i) REQUIRE(s.add(0x4ff40000u + i * 0x10000u, 0x3000, Rw) == Error::Ok);
        CHECK(s.used == 16);
        const PmpSet before = s;
        CHECK(s.add(0x4ff00000u, 0x1000, Rw) == Error::Full);
        CHECK(s.used == 16);
        for (unsigned i = 0; i < PmpSet::Entries; ++i) CHECK(s.addr[i] == before.addr[i]);
        s.clear();
        CHECK(s.used == 0);
        CHECK(s.cfg[0] == 0);
        CHECK_FALSE(s.permits(0x48000000u, 4, PmpSet::Read));
}

TEST_CASE("PmpSet: TOR from 0 in entry 0 takes one entry") {
        PmpSet s;
        CHECK(s.entriesFor(0, 0x3000) == 1);
        REQUIRE(s.add(0, 0x3000, PmpSet::Read) == Error::Ok);
        CHECK(s.used == 1);
        CHECK(s.permits(0, 4, PmpSet::Read));
        CHECK_FALSE(s.permits(0x3000, 4, PmpSet::Read));
}

TEST_CASE("PmpSet: extent and allows, across adjacent entries") {
        PmpSet s;
        // A program's main range: code (R X), the straddling granule (R W X), data (R W).
        REQUIRE(s.add(0x4a000000u, 0x1000, Rx) == Error::Ok);
        REQUIRE(s.add(0x4a001000u, 0x80, Rwx) == Error::Ok);
        REQUIRE(s.add(0x4a001080u, 0x10000 - 0x1080, Rw) == Error::Ok);
        CHECK(s.extent(0x4a000ff0u, PmpSet::Read) == 0x10);
        CHECK(s.extent(0x4a000ff0u, PmpSet::Write) == 0);
        CHECK(s.extent(0x4a001000u, PmpSet::Write) == 0x80);
        CHECK(s.extent(0x4a00fffcu, PmpSet::Write) == 4);
        CHECK(s.extent(0x4a010000u, PmpSet::Read) == 0);
        CHECK(s.extent(0x4ff40000u, PmpSet::Read) == 0);
        // Reading from code into data: each access would pass, so the range does.
        CHECK(s.allows(0x4a000f00u, 0x400, PmpSet::Read));
        CHECK_FALSE(s.permits(0x4a000f00u, 0x400, PmpSet::Read)); // one access couldn't
        CHECK_FALSE(s.allows(0x4a000f00u, 0x400, PmpSet::Write));
        CHECK(s.allows(0x4a001000u, 0x200, PmpSet::Write));
        CHECK_FALSE(s.allows(0x4a00ff00u, 0x200, PmpSet::Read)); // past the end
        CHECK_FALSE(s.allows(0xfffffff0u, 0x20, PmpSet::Read));  // wraps
        CHECK(s.allows(0x4ff40000u, 0, PmpSet::Write));          // empty
}
