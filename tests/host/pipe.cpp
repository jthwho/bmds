/**
 * @file      pipe.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <bmds/interrupt.h>
#include <bmds/pipe.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::Interrupt;
using bmds::Pipe;
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

        void pause(int ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }

} // namespace

TEST_CASE("Pipe: create checks the size") {
        auto [ends, err] = Pipe::create(1000);
        CHECK(err == Error::Invalid);
}

TEST_CASE("Pipe: data flows and EOF follows the writer's close") {
        const size_t base = HostMemory::liveAllocations();
        {
                auto [ends, err] = Pipe::create(64);
                REQUIRE(err.isOk());
                CHECK(ends.read->canRead());
                CHECK_FALSE(ends.read->canWrite());
                CHECK(ends.write->canWrite());

                auto [n, werr] = ends.write->write(bmds::StringView("hello"), WaitForever);
                CHECK(werr.isOk());
                CHECK(n == 5);
                // The ends are mode-checked.
                uint8_t b[4];
                CHECK(ends.write->read(WritableByteSpan(b), WaitForever).error() == Error::BadDescriptor);
                CHECK(ends.read->write(bmds::StringView("x"), WaitForever).error() == Error::BadDescriptor);

                ends.write.reset();
                CHECK(readAll(*ends.read) == "hello");
        }
        CHECK(HostMemory::liveAllocations() == base); // pipe and handles freed
}

TEST_CASE("Pipe: a writer bigger than the ring streams through a concurrent reader") {
        const size_t base = HostMemory::liveAllocations();
        {
                auto [ends, err] = Pipe::create(64);
                REQUIRE(err.isOk());
                std::string data;
                for (int i = 0; i < 10000; ++i) data.push_back(static_cast<char>('a' + i % 26));

                std::thread writer([&ends, &data] {
                        FileHandle::Ptr out = std::move(ends.write);
                        auto [n, werr] = out->write(bmds::StringView(data.data(), data.size()), WaitForever);
                        CHECK(werr.isOk());
                        CHECK(n == data.size());
                });
                CHECK(readAll(*ends.read) == data);
                writer.join();
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Pipe: closing the reader breaks the pipe for a blocked writer") {
        auto [ends, err] = Pipe::create(64);
        REQUIRE(err.isOk());
        std::string big(1000, 'x');
        Error       writeErr;
        size_t      written = 0;
        std::thread writer([&] {
                auto [n, e] = ends.write->write(bmds::StringView(big.data(), big.size()), WaitForever);
                written = n;
                writeErr = e;
        });
        pause(50); // let it fill the ring and block
        ends.read.reset();
        writer.join();
        CHECK(writeErr == Error::BrokenPipe);
        CHECK(written == 64);
        auto [n, e] = ends.write->write(bmds::StringView("more"), WaitForever);
        CHECK(e == Error::BrokenPipe);
        CHECK(n == 0);
}

TEST_CASE("Pipe: timeouts") {
        auto [ends, err] = Pipe::create(64);
        REQUIRE(err.isOk());
        uint8_t buf[8];
        CHECK(ends.read->read(WritableByteSpan(buf), 20).error() == Error::Timeout);
        std::string big(100, 'x');
        auto [n, e] = ends.write->write(bmds::StringView(big.data(), big.size()), 20);
        CHECK(e == Error::Timeout);
        CHECK(n == 64);
}

TEST_CASE("Pipe: the caller's interrupt stops blocked readers and writers") {
        Interrupt intr;
        auto [ends, err] = Pipe::create(64);
        REQUIRE(err.isOk());
        auto [full, ferr] = Pipe::create(64);
        REQUIRE(ferr.isOk());
        std::string big(100, 'x');

        Error       readErr, writeErr, otherErr;
        std::thread reader([&] {
                Interrupt::Scope scope(&intr);
                uint8_t          buf[8];
                readErr = ends.read->read(WritableByteSpan(buf), WaitForever).error();
        });
        std::thread writer([&] {
                Interrupt::Scope scope(&intr);
                writeErr = full.write->write(bmds::StringView(big.data(), big.size()), WaitForever).error();
        });
        // A caller with another interrupt (none) isn't stopped by this one.
        std::thread other([&] {
                uint8_t buf[8];
                otherErr = ends.read->read(WritableByteSpan(buf), 200).error();
        });
        pause(50);
        intr.raise();
        reader.join();
        writer.join();
        other.join();
        CHECK(readErr == Error::Interrupted);
        CHECK(writeErr == Error::Interrupted);
        CHECK(otherErr == Error::Timeout);
}

TEST_CASE("Pipe: allocation failure") {
        const size_t base = HostMemory::liveAllocations();
        for (long successes = 0; successes < 3; ++successes) {
                HostMemory::FailGuard fail(successes);
                auto [ends, err] = Pipe::create(64);
                CHECK(err == Error::NoMem);
        }
        CHECK(HostMemory::liveAllocations() == base);
}
