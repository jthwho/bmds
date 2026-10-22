/**
 * @file      streampair.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <bmds/stream.h>
#include <bmds/streampair.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::FileHandle;
using bmds::Stream;
using bmds::StreamPair;
using bmds::WaitForever;
using bmds::WritableByteSpan;

namespace {

        std::string readAll(FileHandle &in) {
                std::string out;
                uint8_t     buf[100];
                for (;;) {
                        auto [n, err] = in.read(WritableByteSpan(buf), WaitForever);
                        if (err == Error::EndOfFile) break;
                        REQUIRE(err.isOk());
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
                return out;
        }

} // namespace

TEST_CASE("StreamPair: create checks the size") {
        CHECK(StreamPair::create(1000).error() == Error::Invalid);
}

TEST_CASE("StreamPair: each end reads what the other writes, both ways; closing ends the other's reads") {
        const size_t base = HostMemory::liveAllocations();
        {
                auto [ends, err] = StreamPair::create(64);
                REQUIRE(err.isOk());
                CHECK(ends.first->canRead());
                CHECK(ends.first->canWrite());
                CHECK((ends.first->stream().readiness() & Stream::Readable) == 0);
                CHECK((ends.first->stream().readiness() & Stream::Writable) != 0);
                CHECK(ends.first->write(bmds::StringView("ping"), WaitForever).value() == 4);
                CHECK(ends.second->write(bmds::StringView("pong!"), WaitForever).value() == 5);
                CHECK((ends.second->stream().readiness() & Stream::Readable) != 0);
                uint8_t buf[16];
                auto [n, rerr] = ends.second->read(WritableByteSpan(buf), WaitForever);
                CHECK(rerr.isOk());
                CHECK(std::string(reinterpret_cast<const char *>(buf), n) == "ping");
                ends.second.reset(); // what it wrote stays readable, then the end
                CHECK((ends.first->stream().readiness() & Stream::HungUp) != 0);
                CHECK(readAll(*ends.first) == "pong!");
                CHECK(ends.first->write(bmds::StringView("x"), WaitForever).error() == Error::BrokenPipe);
        }
        CHECK(HostMemory::liveAllocations() == base); // the pair and its handles freed
}

TEST_CASE("StreamPair: more than a ring's worth streams through a concurrent reader, each way") {
        const size_t base = HostMemory::liveAllocations();
        {
                auto [ends, err] = StreamPair::create(64);
                REQUIRE(err.isOk());
                std::string big;
                for (int i = 0; i < 1000; ++i) big += char('a' + i % 26);
                std::thread writer([&] {
                        CHECK(ends.first->write(bmds::StringView(big.data(), big.size()), WaitForever).value() ==
                              big.size());
                        ends.first.reset();
                });
                CHECK(readAll(*ends.second) == big);
                writer.join();
        }
        CHECK(HostMemory::liveAllocations() == base);
}
