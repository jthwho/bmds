/**
 * @file      fdtable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/fdtable.h>
#include <bmds/memorystream.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::FdTable;
using bmds::FileHandle;
using bmds::NullStream;

namespace {

        // Counts handleClosed() calls.
        class CountingStream final : public bmds::Stream {
                public:
                        bmds::Result<size_t> read(bmds::WritableByteSpan, unsigned int) override {
                                return bmds::makeError<size_t>(Error::EndOfFile);
                        }
                        bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int) override {
                                return bmds::makeResult(data.size());
                        }
                        int     closed = 0;
                        uint8_t lastMode = 0;

                        void handleClosed(uint8_t mode) override {
                                ++closed;
                                lastMode = mode;
                                return;
                        }
        };

        FileHandle::Ptr open(bmds::Stream &s, uint8_t mode) {
                auto [h, err] = FileHandle::open(s, mode);
                REQUIRE(err.isOk());
                return h;
        }

} // namespace

TEST_CASE("FileHandle: reference counting closes on the last reference") {
        const size_t   base = HostMemory::liveAllocations();
        CountingStream stream;
        {
                FileHandle::Ptr a = open(stream, FileHandle::Write);
                CHECK(a->refCount() == 1);
                FileHandle::Ptr b = a;
                CHECK(a->refCount() == 2);
                FileHandle::Ptr c = std::move(b);
                CHECK_FALSE(b);
                CHECK(a->refCount() == 2);
                a.reset();
                CHECK(stream.closed == 0);
                c = c; // self-assignment keeps the reference
                CHECK(c->refCount() == 1);
        }
        CHECK(stream.closed == 1);
        CHECK(stream.lastMode == FileHandle::Write);
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("FileHandle: open validates the mode and reports NoMem") {
        CountingStream stream;
        CHECK(FileHandle::open(stream, 0).error() == Error::Invalid);
        CHECK(FileHandle::open(stream, 4).error() == Error::Invalid);
        HostMemory::FailGuard fail;
        CHECK(FileHandle::open(stream, FileHandle::Read).error() == Error::NoMem);
}

TEST_CASE("FdTable") {
        const size_t   base = HostMemory::liveAllocations();
        CountingStream stream;
        {
                FdTable table;
                CHECK_FALSE(table.isOpen(0));
                CHECK(table.handle(-1) == nullptr);
                CHECK(table.handle(FdTable::MaxFds) == nullptr);

                CHECK(table.set(FdTable::Stdout, open(stream, FileHandle::Write)).isOk());
                CHECK(table.set(FdTable::MaxFds, open(stream, FileHandle::Write)) == Error::BadDescriptor);
                CHECK(stream.closed == 1);                                  // the rejected handle was dropped
                CHECK(table.dup2(FdTable::Stdout, FdTable::Stderr).isOk()); // 2>&1
                CHECK(table.handle(FdTable::Stderr) == table.handle(FdTable::Stdout));
                CHECK(table.handle(1)->refCount() == 2);
                CHECK(table.dup2(5, 1) == Error::BadDescriptor);
                CHECK(table.dup2(1, 1).isOk());

                auto [fd, err] = table.add(open(NullStream::instance(), FileHandle::ReadWrite));
                CHECK(err.isOk());
                CHECK(fd == 0);
                auto [fd3, err3] = table.add(open(NullStream::instance(), FileHandle::Read), 3);
                CHECK(fd3 == 3);
                // dup: the lowest free descriptor at or above the minimum.
                CHECK(table.dup(1, 0).value() == 4);
                CHECK(table.handle(4) == table.handle(1));
                CHECK(table.handle(1)->refCount() == 3);
                CHECK(table.dup(7, 0).error() == Error::BadDescriptor);
                CHECK(table.dup(1, FdTable::MaxFds).error() == Error::BadDescriptor);
                CHECK(table.close(4).isOk());

                // A copy shares handles, and the original is unaffected by
                // changes to it.
                {
                        FdTable copy = table;
                        CHECK(table.handle(1)->refCount() == 4); // fds 1 and 2, in both tables
                        CHECK(copy.close(1).isOk());
                        CHECK(copy.close(1) == Error::BadDescriptor);
                        CHECK(table.isOpen(1));
                }
                CHECK(table.handle(1)->refCount() == 2);

                for (int i = 4; i < FdTable::MaxFds; ++i) {
                        REQUIRE(table.add(open(NullStream::instance(), FileHandle::Read)).isOk());
                }
                CHECK(table.add(open(NullStream::instance(), FileHandle::Read)).error() == Error::TooManyOpen);
                CHECK(table.add(open(NullStream::instance(), FileHandle::Read), -1).error() == Error::BadDescriptor);

                // Close-on-exec: per descriptor, gone with dup, dup2 onto it,
                // close; copied with the table; exec closes the flagged ones.
                {
                        FdTable t;
                        CHECK(t.setCloseOnExec(0, true) == Error::BadDescriptor);
                        REQUIRE(t.set(0, open(NullStream::instance(), FileHandle::Read)).isOk());
                        REQUIRE(t.set(1, open(NullStream::instance(), FileHandle::Write)).isOk());
                        CHECK_FALSE(t.isCloseOnExec(0));
                        CHECK(t.setCloseOnExec(0, true).isOk());
                        CHECK(t.isCloseOnExec(0));
                        CHECK_FALSE(t.isCloseOnExec(t.dup(0).value()));
                        CHECK(t.setCloseOnExec(1, true).isOk());
                        CHECK(t.dup2(0, 1).isOk());
                        CHECK_FALSE(t.isCloseOnExec(1));
                        CHECK(t.dup2(0, 0).isOk());
                        CHECK(t.isCloseOnExec(0));
                        FdTable copy = t;
                        CHECK(copy.isCloseOnExec(0));
                        copy.closeOnExec();
                        CHECK_FALSE(copy.isOpen(0));
                        CHECK(copy.isOpen(1));
                        CHECK(copy.isOpen(2));
                        CHECK(t.isOpen(0));
                        CHECK(t.close(0).isOk());
                        REQUIRE(t.set(0, open(NullStream::instance(), FileHandle::Read)).isOk());
                        CHECK_FALSE(t.isCloseOnExec(0));
                }

                table.closeAll();
                CHECK(stream.closed == 2);
                CHECK_FALSE(table.isOpen(1));
        }
        CHECK(HostMemory::liveAllocations() == base);
}
