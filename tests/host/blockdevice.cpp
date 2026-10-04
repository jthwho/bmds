/**
 * @file      blockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <bmds/ramblockdevice.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::RamBlockDevice;
using bmds::WritableByteSpan;

TEST_CASE("RamBlockDevice") {
        const size_t base = HostMemory::liveAllocations();
        {
                RamBlockDevice dev(512);
                CHECK(dev.sectorSize() == 512);
                CHECK(dev.sectorCount() == 0);
                CHECK(dev.allocate(0, bmds::MemSpace::Psram) == Error::Invalid);
                REQUIRE(dev.allocate(8, bmds::MemSpace::Psram).isOk());
                CHECK(dev.sectorCount() == 8);
                CHECK(dev.isPresent());

                uint8_t out[1024];
                uint8_t in[1024];
                for (size_t i = 0; i < sizeof(out); ++i) out[i] = static_cast<uint8_t>(i * 7);

                SUBCASE("reads back what was written") {
                        CHECK(dev.write(3, ByteSpan(out, 1024)).isOk());
                        CHECK(dev.read(3, WritableByteSpan(in, 1024)).isOk());
                        CHECK(std::memcmp(in, out, 1024) == 0);
                        // Neighbors are untouched (zero-filled).
                        CHECK(dev.read(2, WritableByteSpan(in, 512)).isOk());
                        CHECK(in[0] == 0);
                        CHECK(in[511] == 0);
                        CHECK(dev.contents()[3 * 512 + 1] == 7);
                        CHECK(dev.sync().isOk());
                        CHECK(dev.trim(0, 8).isOk());
                }

                SUBCASE("checks requests") {
                        CHECK(dev.read(0, WritableByteSpan(in, 0)) == Error::Invalid);
                        CHECK(dev.read(0, WritableByteSpan(in, 100)) == Error::Invalid);
                        CHECK(dev.write(0, ByteSpan(out, 513)) == Error::Invalid);
                        CHECK(dev.read(7, WritableByteSpan(in, 1024)) == Error::OutOfRange);
                        CHECK(dev.read(8, WritableByteSpan(in, 512)) == Error::OutOfRange);
                        CHECK(dev.write(UINT64_MAX, ByteSpan(out, 512)) == Error::OutOfRange);
                        CHECK(dev.read(7, WritableByteSpan(in, 512)).isOk());
                        CHECK(dev.trim(4, 5) == Error::OutOfRange);
                }

                SUBCASE("a removed medium is NoMedia") {
                        dev.setPresent(false);
                        CHECK_FALSE(dev.isPresent());
                        CHECK(dev.sectorCount() == 0);
                        CHECK(dev.read(0, WritableByteSpan(in, 512)) == Error::NoMedia);
                        CHECK(dev.write(0, ByteSpan(out, 512)) == Error::NoMedia);
                        CHECK(dev.sync() == Error::NoMedia);
                        dev.setPresent(true);
                        CHECK(dev.read(0, WritableByteSpan(in, 512)).isOk());
                }

                SUBCASE("reallocating replaces the sectors") {
                        CHECK(dev.write(0, ByteSpan(out, 512)).isOk());
                        REQUIRE(dev.allocate(2, bmds::MemSpace::Psram).isOk());
                        CHECK(dev.sectorCount() == 2);
                        CHECK(dev.read(0, WritableByteSpan(in, 512)).isOk());
                        CHECK(in[1] == 0);
                }
        }
        CHECK(HostMemory::liveAllocations() == base);
}
