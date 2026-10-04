/**
 * @file      memorystream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <string>
#include <bmds/memorystream.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::MemoryStream;
using bmds::NullStream;
using bmds::WaitForever;
using bmds::WritableByteSpan;

TEST_CASE("MemoryStream") {
        const size_t base = HostMemory::liveAllocations();
        {
                MemoryStream s(bmds::MemSpace::Psram);
                CHECK_FALSE(s.isTerminal());
                uint8_t buf[8];
                CHECK(s.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);

                CHECK(s.write(bmds::StringView("hello, "), WaitForever).isOk());
                CHECK(s.write(bmds::StringView("world, this is long enough to allocate"), WaitForever).isOk());
                CHECK(s.contents().startsWith("hello, world"));

                auto [n, err] = s.read(WritableByteSpan(buf), WaitForever);
                CHECK(err.isOk());
                CHECK(n == 8);
                CHECK(std::string(reinterpret_cast<char *>(buf), 8) == "hello, w");
                CHECK(s.contents().startsWith("orld"));

                bmds::String taken = s.takeContents();
                CHECK(taken.view().startsWith("orld, this"));
                CHECK(s.contents().isEmpty());
                CHECK(s.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);

                CHECK(s.write(bmds::StringView("abc"), WaitForever).isOk());
                {
                        HostMemory::FailGuard fail;
                        auto [w, werr] = s.write(bmds::StringView("this will not fit inline anymore"), WaitForever);
                        CHECK(werr == Error::NoMem);
                        CHECK(w == 0);
                }
                CHECK(s.contents() == "abc");
                s.clear();
                CHECK(s.contents().isEmpty());
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("NullStream") {
        NullStream &null = NullStream::instance();
        uint8_t     buf[4];
        CHECK(null.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);
        auto [n, err] = null.write(bmds::StringView("gone"), WaitForever);
        CHECK(err.isOk());
        CHECK(n == 4);
}
