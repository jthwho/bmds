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

#include <chrono>
#include <cstring>
#include <string>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/errno.h>
#include <bmds/memorystream.h>
#include <bmds/process.h>
#include <bmds/ramblockdevice.h>
#include <bmds/sys.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"
#include "hostmemory.h"

using bmds::Process;

namespace {

        namespace Vfs = bmds::Vfs;

        // Two small RAM disks, mounted at /pdiskN/p0 for one test and then
        // removed (the registry and mounts are global).
        struct Disks {
                        Disks() : a(512), b(512) {
                                REQUIRE(a.allocate(4 * 2048, bmds::MemSpace::Psram).isOk());
                                REQUIRE(b.allocate(4 * 2048, bmds::MemSpace::Psram).isOk());
                                REQUIRE(Vfs::init().isOk());
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(a, "pdisk", name).isOk());
                                aRoot = std::string("/") + name + "/p0";
                                REQUIRE(bmds::BlockDevices::addNumbered(b, "pdisk", name).isOk());
                                bRoot = std::string("/") + name + "/p0";
                                for (bmds::RamBlockDevice *d : {&a, &b}) {
                                        REQUIRE(Vfs::format(*d, bmds::FsFormat::Auto).isOk());
                                        REQUIRE(Vfs::automount(*d).value() == 1);
                                }
                        }

                        ~Disks() {
                                for (bmds::RamBlockDevice *d : {&a, &b}) {
                                        Vfs::eject(*d);
                                        bmds::BlockDevices::remove(*d);
                                }
                        }

                        bmds::RamBlockDevice a;
                        bmds::RamBlockDevice b;
                        std::string          aRoot;
                        std::string          bRoot;
        };

        bmds_stat_t statOf(Process &p, const char *path) {
                bmds_stat_t st = {};
                st.size = sizeof(st);
                REQUIRE_MESSAGE(p.stat(path, &st) == 0, path);
                return st;
        }

        // Every name in a directory, in the order readdir gives them.
        std::vector<std::string> names(Process &p, const char *path) {
                std::vector<std::string> out;
                const int32_t            dir = p.opendir(path);
                REQUIRE(dir >= 0);
                char name[BMDS_NAME_MAX + 1];
                while (p.readdir(dir, nullptr, name, sizeof(name)) > 0) out.emplace_back(name);
                CHECK(p.closedir(dir) == 0);
                return out;
        }

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

TEST_CASE("Process: files and directories") {
        { Disks warm; } // the VFS's and the device registry's one-time allocations aren't the process's
        const size_t base = HostMemory::liveAllocations();
        {
                Disks         disks;
                bmds::FdTable fds;
                Process       p(fds);
                REQUIRE(p.setCwd(disks.aRoot.c_str()).isOk());
                char buf[BMDS_PATH_MAX + 2];

                // Paths and flags the kernel refuses.
                std::string tooLong(BMDS_PATH_MAX + 1, 'x');
                CHECK(p.open(nullptr, BMDS_O_READ) == -BMDS_EFAULT);
                CHECK(p.open("", BMDS_O_READ) == -BMDS_ENOENT);
                CHECK(p.open(tooLong.c_str(), BMDS_O_READ) == -BMDS_ENAMETOOLONG);
                CHECK(p.open("f", 0) == -BMDS_EINVAL);
                CHECK(p.open("f", BMDS_O_WRITE | 0x40) == -BMDS_EINVAL);
                CHECK(p.open("missing", BMDS_O_READ) == -BMDS_ENOENT);

                // A file: relative to the cwd, on the lowest free fd.
                int32_t fd = p.open("f.txt", BMDS_O_WRITE | BMDS_O_CREATE | BMDS_O_EXCL);
                CHECK(fd == 0);
                CHECK(p.write(fd, "0123456789", 10) == 10);
                bmds_stat_t st = {};
                st.size = sizeof(st);
                CHECK(p.fstat(fd, &st) == 0);
                CHECK(st.type == BMDS_TYPE_FILE);
                CHECK(st.file_size == 10);
                CHECK(st.open_flags == BMDS_O_WRITE);
                CHECK(p.fstat(7, &st) == -BMDS_EBADF);
                CHECK(p.close(fd) == 0);
                CHECK(p.open("f.txt", BMDS_O_WRITE | BMDS_O_CREATE | BMDS_O_EXCL) == -BMDS_EEXIST);
                st = statOf(p, (disks.aRoot + "/f.txt").c_str());
                CHECK(st.type == BMDS_TYPE_FILE);
                CHECK(st.file_size == 10);
                CHECK(st.modified_ns != 0);
                CHECK(st.open_flags == 0);

                // A caller's struct smaller than the kernel's gets only what fits.
                bmds_stat_t small;
                std::memset(&small, 0xEE, sizeof(small));
                small.size = 12; // size, type, attributes
                CHECK(p.stat("f.txt", &small) == 0);
                CHECK(small.size == 12);
                CHECK(small.type == BMDS_TYPE_FILE);
                CHECK(small.open_flags == 0xEEEEEEEEu);
                small.size = 2;
                CHECK(p.stat("f.txt", &small) == -BMDS_EINVAL);
                CHECK(p.stat("f.txt", nullptr) == -BMDS_EFAULT);

                // Directories and the current directory.
                CHECK(p.mkdir("d") == 0);
                CHECK(p.mkdir("d") == -BMDS_EEXIST);
                CHECK(p.mkdir("no/such") == -BMDS_ENOENT);
                CHECK(p.rename("f.txt", "d/g.txt") == 0);
                CHECK(p.rename("missing", "x") == -BMDS_ENOENT);
                CHECK(p.rename("d/g.txt", (disks.bRoot + "/g.txt").c_str()) == -BMDS_EXDEV);
                CHECK(p.unlink("d") == -BMDS_EISDIR);
                CHECK(p.rmdir("d") == -BMDS_ENOTEMPTY);
                CHECK(p.chdir("d") == 0);
                CHECK(p.getcwd(buf, sizeof(buf)) == static_cast<int32_t>(disks.aRoot.size() + 2));
                CHECK(std::string(buf) == disks.aRoot + "/d");
                CHECK(p.getcwd(buf, static_cast<uint32_t>(disks.aRoot.size() + 2)) == -BMDS_ERANGE);
                CHECK(p.getcwd(nullptr, 10) == -BMDS_EFAULT);
                CHECK(p.chdir("g.txt") == -BMDS_ENOTDIR);
                CHECK(p.chdir("nowhere") == -BMDS_ENOENT);
                CHECK(p.open("g.txt", BMDS_O_READ) == 0);
                CHECK(p.read(0, buf, sizeof(buf)) == 10);
                CHECK(p.unlink("g.txt") == -BMDS_EBUSY); // open files stay
                CHECK(p.close(0) == 0);
                CHECK(p.chdir("..") == 0);
                CHECK(std::string(p.cwd().data(), p.cwd().size()) == disks.aRoot);
                CHECK(p.open("d", BMDS_O_READ) == -BMDS_EISDIR);

                // Reading a directory: names, what they are, cut names.
                REQUIRE(p.close(p.open("d/a-rather-long-name.txt", BMDS_O_WRITE | BMDS_O_CREATE)) == 0);
                REQUIRE(p.close(p.open("d/h\xc3\xa9llo", BMDS_O_WRITE | BMDS_O_CREATE)) == 0);
                REQUIRE(p.mkdir("d/sub") == 0);
                CHECK(names(p, "d") ==
                      std::vector<std::string>{"g.txt", "a-rather-long-name.txt", "h\xc3\xa9llo", "sub"});
                int32_t dir = p.opendir("d");
                REQUIRE(dir == 0);
                bmds_stat_t info = {};
                info.size = sizeof(info);
                char name[8];
                CHECK(p.readdir(dir, &info, name, sizeof(name)) == 5);
                CHECK(std::string(name) == "g.txt");
                CHECK(info.type == BMDS_TYPE_FILE);
                CHECK(info.file_size == 10);
                CHECK(p.readdir(dir, &info, name, 5) == 22); // cut, with its full length
                CHECK(std::string(name) == "a-ra");
                CHECK(p.readdir(dir, nullptr, name, 3) == 6); // not inside the é
                CHECK(std::string(name) == "h");
                CHECK(p.readdir(dir, &info, nullptr, 0) == 3);
                CHECK(info.type == BMDS_TYPE_DIRECTORY);
                CHECK(p.readdir(dir, &info, name, sizeof(name)) == 0);
                CHECK(p.closedir(dir) == 0);
                CHECK(p.closedir(dir) == -BMDS_EBADF);
                CHECK(p.readdir(dir, nullptr, name, sizeof(name)) == -BMDS_EBADF);
                CHECK(p.opendir("d/g.txt") == -BMDS_ENOTDIR);
                CHECK(p.opendir("nowhere") == -BMDS_ENOENT);
                for (int i = 0; i < Process::MaxDirectories; ++i) CHECK(p.opendir(".") == i);
                CHECK(p.opendir(".") == -BMDS_EMFILE);
                CHECK(p.openDirectories() == Process::MaxDirectories);

                // Times, devices, removing.
                const int64_t when = bmds::Timestamp::fromSeconds(1700000000).ns;
                CHECK(p.setMtime("d/g.txt", when) == 0);
                CHECK(statOf(p, "d/g.txt").modified_ns == when);
                CHECK(p.setMtime("nowhere", when) == -BMDS_ENOENT);
                CHECK(statOf(p, "/dev/null").type == BMDS_TYPE_DEVICE);
                CHECK(statOf(p, "/").type == BMDS_TYPE_DIRECTORY);
                fd = p.open("/dev/null", BMDS_O_WRITE);
                CHECK(fd >= 0);
                st.size = sizeof(st);
                CHECK(p.fstat(fd, &st) == 0);
                CHECK(st.type == BMDS_TYPE_DEVICE);
                CHECK(p.unlink("d/g.txt") == 0);
                CHECK(p.unlink("d/g.txt") == -BMDS_ENOENT);

                // Descriptors.
                CHECK(p.dup(fd, 5) == 5);
                CHECK(p.dup(fd, 0) == 1);
                CHECK(p.dup(9, 0) == -BMDS_EBADF);
                CHECK(p.dup(fd, 99) == -BMDS_EINVAL);
                CHECK(p.dup2(fd, 12) == 12);
                CHECK(p.dup2(fd, 16) == -BMDS_EBADF);
                CHECK(p.dup2(9, 3) == -BMDS_EBADF);
                CHECK(p.write(12, "x", 1) == 1);

                // Everything goes when the program ends.
                p.release();
                CHECK(p.openDirectories() == 0);
                CHECK(p.write(12, "x", 1) == -BMDS_EBADF);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Process: console input, lines and raw") {
        const size_t base = HostMemory::liveAllocations();
        {
                FakeConsole        console;
                bmds::MemoryStream file(bmds::MemSpace::Psram);
                bmds::FdTable      fds = tableWith(console, console, file);
                Process            p(fds, &console, &console.interrupt());
                char               buf[64];

                // Lines: echoed and edited, given whole (or in pieces to a small read).
                console.input = "hellp\x7fo\rworld\r\n\x04";
                CHECK(p.read(0, buf, sizeof(buf)) == 6);
                CHECK(std::string(buf, 6) == "hello\n");
                CHECK(console.take().find("hellp") == 0); // echoed, and the backspace too
                CHECK(p.read(0, buf, 3) == 3);
                CHECK(std::string(buf, 3) == "wor");
                CHECK(p.read(0, buf, sizeof(buf)) == 3);
                CHECK(std::string(buf, 3) == "ld\n");
                CHECK(p.read(0, buf, sizeof(buf)) == 0); // Ctrl-D on an empty line, not an empty line for the LF
                CHECK(p.read(0, buf, sizeof(buf)) == 0); // and the console has no more

                // The terminal: modes, size, colors; only on the console.
                bmds_term_t t = {};
                t.size = sizeof(t);
                CHECK(p.termGet(1, &t) == 0);
                CHECK(t.flags ==
                      (BMDS_TERM_SIGNALS | BMDS_TERM_ONLCR | BMDS_TERM_COLORS | BMDS_TERM_LOGS | BMDS_TERM_LINE));
                CHECK(t.columns == 80);
                CHECK(t.rows == 24);
                CHECK(t.colors == 16777216u);
                CHECK(p.termGet(2, &t) == -BMDS_ENOTTY);
                CHECK(p.termGet(5, &t) == -BMDS_EBADF);
                CHECK(p.termSet(2, 0) == -BMDS_ENOTTY);
                CHECK(p.termSet(0, 0x100) == -BMDS_EINVAL);

                // Raw: bytes as typed, after what was typed behind a line.
                console.input += "one\rtwo";
                CHECK(p.read(0, buf, sizeof(buf)) == 4);
                CHECK(p.termSet(0, BMDS_TERM_ONLCR) == 0);
                CHECK_FALSE(console.mode().signals);
                CHECK_FALSE(console.mode().colors);
                CHECK(console.mode().nlToCrNl);
                console.take();
                CHECK(p.read(0, buf, sizeof(buf)) == 3);
                CHECK(std::string(buf, 3) == "two");
                console.input += "\x03\x1b[A";
                CHECK(p.read(0, buf, sizeof(buf)) == 4);
                CHECK(std::string(buf, 4) == "\x03\x1b[A");
                CHECK(console.take().empty()); // no echo
                CHECK(p.termGet(0, &t) == 0);
                CHECK(t.flags == BMDS_TERM_ONLCR);

                // Ctrl-C: reads, sleeps and interrupted() see it.
                CHECK(p.termSet(0, BMDS_TERM_SIGNALS | BMDS_TERM_LINE) == 0);
                CHECK(p.interrupted() == 0);
                console.interrupt().raise();
                CHECK(p.interrupted() == 1);
                CHECK(p.read(0, buf, sizeof(buf)) == -BMDS_EINTR);
                CHECK(p.sleep(1'000'000'000) == -BMDS_EINTR);
                CHECK(p.sleep(0) == -BMDS_EINTR);
                console.interrupt().clear();

                // A file isn't the terminal.
                CHECK(p.write(2, "x", 1) == 1);
                CHECK(p.read(2, buf, sizeof(buf)) == -BMDS_EBADF);
                p.release();
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Process: clocks, sleeping, memory") {
        bmds::FdTable fds;
        Process       p(fds);
        int64_t       a = 0, b = 0, wall = 0;
        CHECK(p.clockGet(BMDS_CLOCK_MONOTONIC, &a) == 0);
        CHECK(p.clockGet(BMDS_CLOCK_REALTIME, &wall) == 0);
        CHECK(p.clockGet(2, &wall) == -BMDS_EINVAL);
        CHECK(p.clockGet(BMDS_CLOCK_REALTIME, nullptr) == -BMDS_EFAULT);
        CHECK(p.sleep(3'000'000) == 0); // no interrupt: still sleeps
        CHECK(p.sleep(-5) == 0);
        CHECK(p.clockGet(BMDS_CLOCK_MONOTONIC, &b) == 0);
        CHECK(b - a >= 3'000'000);
        CHECK(p.interrupted() == 0);

        void           *m = p.memAlloc(1000, 0, 0);
        bmds_mem_info_t info = {};
        info.size = sizeof(info);
        CHECK(p.memInfo(0, &info) == 0);
        CHECK(info.program_bytes == 1000);
        CHECK(info.program_blocks == 1);
        CHECK(p.memInfo(0x80, &info) == -BMDS_EINVAL);
        CHECK(p.memInfo(0, nullptr) == -BMDS_EFAULT);
        p.memFree(m);
}
