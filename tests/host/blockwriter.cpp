/**
 * @file      blockwriter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstdint>
#include <vector>
#include <bmds/blockwriter.h>
#include "hostmemory.h"

using bmds::BlockWriter;
using bmds::ByteSpan;
using bmds::Error;
using bmds::Result;
using bmds::WritableByteSpan;

namespace {

        // A file that records each write: where it started and how long it was.
        class Recorder final : public bmds::Stream {
                public:
                        explicit Recorder(uint64_t start) : position(start) {}

                        Result<size_t> read(WritableByteSpan, unsigned int) override {
                                return bmds::makeError<size_t>(Error::NotSupported);
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                if (failAfter == 0) return bmds::makeError<size_t>(Error::IoError);
                                --failAfter;
                                const size_t n = data.size() < most ? data.size() : most;
                                writes.push_back({position, n});
                                bytes.insert(bytes.end(), data.data(), data.data() + n);
                                position += n;
                                return bmds::makeResult(n);
                        }

                        struct Write {
                                        uint64_t at;
                                        size_t   size;
                        };

                        uint64_t             position;
                        size_t               most = SIZE_MAX; ///< Taken at most per call.
                        unsigned             failAfter = ~0u; ///< Calls before writes fail.
                        std::vector<Write>   writes;
                        std::vector<uint8_t> bytes;
        };

        std::vector<uint8_t> pattern(size_t n) {
                std::vector<uint8_t> v(n);
                for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + i / 251);
                return v;
        }

        // Feeds @p data in pieces of @p piece bytes.
        Error feed(BlockWriter &w, Recorder &file, const std::vector<uint8_t> &data, size_t piece) {
                for (size_t at = 0; at < data.size(); at += piece) {
                        const size_t n = data.size() - at < piece ? data.size() - at : piece;
                        const Error  err = w.write(file, ByteSpan(data.data() + at, n));
                        if (err.isError()) return err;
                }
                return w.flush(file);
        }

} // namespace

TEST_CASE("BlockWriter gathers pieces into aligned blocks") {
        const size_t base = HostMemory::liveAllocations();
        {
                const std::vector<uint8_t> data = pattern(300000);
                BlockWriter                w;
                Recorder                   file(0);
                w.reset(0);
                REQUIRE(feed(w, file, data, 16000).isOk());
                CHECK(file.bytes == data);
                REQUIRE(file.writes.size() == 5);
                for (size_t i = 0; i < 4; ++i) {
                        CHECK(file.writes[i].at == i * BlockWriter::BlockBytes);
                        CHECK(file.writes[i].size == BlockWriter::BlockBytes);
                }
                CHECK(file.writes[4].size == data.size() - 4 * BlockWriter::BlockBytes);
                CHECK(w.flush(file).isOk()); // nothing left
                CHECK(file.writes.size() == 5);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("BlockWriter resumed mid-file reaches a block's boundary first") {
        const std::vector<uint8_t> data = pattern(200000);
        BlockWriter                w;
        Recorder                   file(1000);
        w.reset(1000);
        REQUIRE(feed(w, file, data, 4099).isOk());
        CHECK(file.bytes == data);
        REQUIRE(file.writes.size() >= 2);
        CHECK(file.writes[0].size == BlockWriter::BlockBytes - 1000);
        CHECK(file.writes[1].at == BlockWriter::BlockBytes);
        CHECK(file.writes[1].size == BlockWriter::BlockBytes);
}

TEST_CASE("BlockWriter: a piece bigger than a block, a stream taking part of each write") {
        const std::vector<uint8_t> data = pattern(3 * BlockWriter::BlockBytes + 17);
        BlockWriter                w;
        Recorder                   file(0);
        file.most = 5000;
        w.reset(0);
        REQUIRE(feed(w, file, data, data.size()).isOk());
        CHECK(file.bytes == data);
}

TEST_CASE("BlockWriter: a failed write stops there") {
        const std::vector<uint8_t> data = pattern(4 * BlockWriter::BlockBytes);
        BlockWriter                w;
        Recorder                   file(0);
        file.failAfter = 2;
        w.reset(0);
        CHECK(feed(w, file, data, 10000) == Error::IoError);
        CHECK(file.bytes.size() == 2 * BlockWriter::BlockBytes);
        // Started again from the start, as a fetch does when a server ignores its range.
        Recorder again(0);
        w.reset(0);
        REQUIRE(feed(w, again, data, 10000).isOk());
        CHECK(again.bytes == data);
}
