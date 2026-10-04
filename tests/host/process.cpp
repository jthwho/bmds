/**
 * @file      process.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Process: a program's system calls and the resources it owns.
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <bmds/errno.h>
#include <bmds/memorystream.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "fakeconsole.h"
#include "hostmemory.h"

using bmds::Process;

namespace {

        bmds::FdTable tableWith(bmds::Stream &in, bmds::Stream &out, bmds::Stream &err) {
                bmds::FdTable fds;
                REQUIRE(fds.set(0, bmds::FileHandle::open(in, bmds::FileHandle::Read).value()).isOk());
                REQUIRE(fds.set(1, bmds::FileHandle::open(out, bmds::FileHandle::Write).value()).isOk());
                REQUIRE(fds.set(2, bmds::FileHandle::open(err, bmds::FileHandle::Write).value()).isOk());
                return fds;
        }

} // namespace

TEST_CASE("Process: memory") {
        const size_t base = HostMemory::liveAllocations();
        {
                bmds::FdTable fds;
                Process       p(fds);
                for (uint32_t align : {0u, 8u, 16u, 64u, 4096u}) {
                        CAPTURE(align);
                        void *m = p.memAlloc(100, align, 0);
                        REQUIRE(m != nullptr);
                        const uint32_t want = align < BMDS_MEM_ALIGN ? BMDS_MEM_ALIGN : align;
                        CHECK(reinterpret_cast<uintptr_t>(m) % want == 0);
                        std::memset(m, 0x5A, 100);
                }
                CHECK(p.allocations() == 5);
                CHECK(p.allocatedBytes() == 500);
                CHECK(p.memAlloc(16, 3, 0) == nullptr);    // not a power of two
                CHECK(p.memAlloc(16, 8192, 0) == nullptr); // too strict
                CHECK(p.memAlloc(16, 0, 0x80) == nullptr); // unknown flag
                CHECK(p.memAlloc(16, 0, BMDS_MEM_FAST | BMDS_MEM_DMA) != nullptr);
                // Freeing in any order, and realloc keeping the contents.
                void *a = p.memAlloc(10, 0, 0);
                void *b = p.memAlloc(20, 0, 0);
                std::memcpy(a, "0123456789", 10);
                p.memFree(b);
                a = p.memRealloc(a, 1000, 0);
                REQUIRE(a != nullptr);
                CHECK(std::memcmp(a, "0123456789", 10) == 0);
                a = p.memRealloc(a, 4, 0);
                CHECK(std::memcmp(a, "0123", 4) == 0);
                CHECK(p.memRealloc(a, 4, 0x80) == nullptr); // unknown flag: the old block stays
                p.memFree(nullptr);
                void *c = p.memRealloc(nullptr, 8, 0);
                CHECK(c != nullptr);
                CHECK(p.allocations() == 8);
                // Whatever is left goes when the program ends.
                p.release();
                CHECK(p.allocations() == 0);
                CHECK(p.allocatedBytes() == 0);
                CHECK(HostMemory::liveAllocations() == base);
                // And a released process can allocate again (the destructor frees it).
                CHECK(p.memAlloc(64, 0, 0) != nullptr);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Process: file descriptors") {
        bmds::MemoryStream in(bmds::MemSpace::Psram), out(bmds::MemSpace::Psram);
        FakeConsole        console;
        REQUIRE(in.write(bmds::StringView("input"), bmds::WaitForever).error().isOk());
        bmds::FdTable fds = tableWith(in, out, console);
        Process       p(fds);
        char          buf[16] = {};
        CHECK(p.read(0, buf, sizeof(buf)) == 5);
        CHECK(std::string(buf, 5) == "input");
        CHECK(p.read(0, buf, sizeof(buf)) == 0); // end of file
        CHECK(p.read(0, buf, 0) == 0);
        CHECK(p.write(1, "out", 3) == 3);
        CHECK(std::string(out.contents().data(), out.contents().size()) == "out");
        CHECK(p.write(0, "x", 1) == -BMDS_EBADF); // stdin isn't open for writing
        CHECK(p.read(9, buf, 1) == -BMDS_EBADF);
        CHECK(p.read(-1, buf, 1) == -BMDS_EBADF);
        CHECK(p.isatty(2) == 1);
        CHECK(p.isatty(1) == 0);
        int64_t pos = -1;
        CHECK(p.seek(1, 0, BMDS_SEEK_SET, &pos) == -BMDS_ESPIPE);
        CHECK(p.seek(1, 0, 7, &pos) == -BMDS_EINVAL);
        // Closing is the program's: the shell's table is untouched.
        CHECK(p.close(1) == 0);
        CHECK(p.close(1) == -BMDS_EBADF);
        CHECK(fds.isOpen(1));
        p.release();
        CHECK(p.write(2, "x", 1) == -BMDS_EBADF);
        fds.closeAll();
}

TEST_CASE("Process: arguments and environment") {
        const size_t base = HostMemory::liveAllocations();
        {
                bmds::FdTable          fds;
                Process                p(fds);
                const bmds::StringView args[] = {"./prog", "one", "two words"};
                const bmds::StringView env[] = {"HOME=/sd0/p0", "X="};
                REQUIRE(p.setArguments(args, env).isOk());
                CHECK(p.argc() == 3);
                CHECK(std::string(p.argv()[0]) == "./prog");
                CHECK(std::string(p.argv()[2]) == "two words");
                CHECK(p.argv()[3] == nullptr);
                CHECK(std::string(p.envp()[0]) == "HOME=/sd0/p0");
                CHECK(std::string(p.envp()[1]) == "X=");
                CHECK(p.envp()[2] == nullptr);
                REQUIRE(p.setArguments(bmds::Span<const bmds::StringView>(), bmds::Span<const bmds::StringView>())
                                .isOk());
                CHECK(p.argc() == 0);
                CHECK(p.argv()[0] == nullptr);
                CHECK(p.envp()[0] == nullptr);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Process: errors map to the ABI's numbers") {
        CHECK(Process::errnoFor(bmds::Error::Ok) == 0);
        CHECK(Process::errnoFor(bmds::Error::NotExist) == -BMDS_ENOENT);
        CHECK(Process::errnoFor(bmds::Error::Interrupted) == -BMDS_EINTR);
        CHECK(Process::errnoFor(bmds::Error::BrokenPipe) == -BMDS_EPIPE);
        CHECK(Process::errnoFor(bmds::Error::NoMedia) == -BMDS_ENOMEDIUM);
        CHECK(Process::errnoFor(bmds::Error::BadDescriptor) == -BMDS_EBADF);
        CHECK(Process::errnoFor(bmds::Error::DeviceError) == -BMDS_EIO);
}
