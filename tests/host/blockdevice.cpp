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
                        CHECK(dev.sync().isOk());
                        CHECK(dev.trim(0, 8).isOk());
                        // Trimmed whole: the chunk is gone, the sectors read as zeros.
                        CHECK(dev.usedBytes() == 0);
                        CHECK(dev.read(3, WritableByteSpan(in, 512)).isOk());
                        CHECK(in[1] == 0);
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

TEST_CASE("RamBlockDevice: sparse") {
        const size_t base = HostMemory::liveAllocations();
        {
                // 512 MB nominal: only the chunk table is allocated.
                RamBlockDevice dev(512);
                REQUIRE(dev.allocate(1024 * 1024, bmds::MemSpace::Psram).isOk());
                CHECK(dev.usedBytes() == 0);
                constexpr uint64_t PerChunk = RamBlockDevice::ChunkBytes / 512;
                uint8_t            zeros[1024] = {};
                uint8_t            data[1024];
                uint8_t            in[1024];
                for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i + 1);
                // Zeros into an unwritten chunk cost nothing.
                CHECK(dev.write(100, ByteSpan(zeros, 1024)).isOk());
                CHECK(dev.usedBytes() == 0);
                // Across a chunk boundary, far out: two chunks.
                const uint64_t at = 1000 * PerChunk - 1;
                CHECK(dev.write(at, ByteSpan(data, 1024)).isOk());
                CHECK(dev.usedBytes() == 2 * RamBlockDevice::ChunkBytes);
                CHECK(dev.read(at, WritableByteSpan(in, 1024)).isOk());
                CHECK(std::memcmp(in, data, 1024) == 0);
                CHECK(dev.read(at - 1, WritableByteSpan(in, 512)).isOk());
                CHECK(in[0] == 0);
                // Zeros into a written chunk are written.
                CHECK(dev.write(at, ByteSpan(zeros, 512)).isOk());
                CHECK(dev.read(at, WritableByteSpan(in, 512)).isOk());
                CHECK(in[5] == 0);
                // Trimmed sectors read as zeros; a chunk is freed once it is all
                // zeros, whether trimmed whole or in pieces.
                CHECK(dev.trim(at + 1, PerChunk - 1).isOk());
                CHECK(dev.usedBytes() == RamBlockDevice::ChunkBytes);
                CHECK(dev.read(at + 1, WritableByteSpan(in, 512)).isOk());
                CHECK(in[0] == 0);
                CHECK(dev.write(at + 1, ByteSpan(data, 1024)).isOk());
                CHECK(dev.usedBytes() == 2 * RamBlockDevice::ChunkBytes);
                CHECK(dev.trim(at + 2, 1).isOk()); // half of what's there
                CHECK(dev.usedBytes() == 2 * RamBlockDevice::ChunkBytes);
                CHECK(dev.read(at + 1, WritableByteSpan(in, 1024)).isOk());
                CHECK(in[0] == 1);
                CHECK(in[512] == 0);
                CHECK(dev.trim(at + 1, 1).isOk());
                CHECK(dev.usedBytes() == RamBlockDevice::ChunkBytes);
                CHECK(dev.trim(0, dev.sectorCount()).isOk());
                CHECK(dev.usedBytes() == 0);
                // A short last chunk: the device's end counts as covering it.
                RamBlockDevice small(512);
                REQUIRE(small.allocate(PerChunk + 10, bmds::MemSpace::Psram).isOk());
                CHECK(small.write(PerChunk + 9, ByteSpan(data, 512)).isOk());
                CHECK(small.usedBytes() == RamBlockDevice::ChunkBytes);
                CHECK(small.trim(PerChunk, 9).isOk());
                CHECK(small.usedBytes() == RamBlockDevice::ChunkBytes);
                CHECK(small.trim(PerChunk, 10).isOk());
                CHECK(small.usedBytes() == 0);
        }
        CHECK(HostMemory::liveAllocations() == base);
}
