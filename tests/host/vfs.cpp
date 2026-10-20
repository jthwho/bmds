/**
 * @file      vfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <algorithm>
#include <map>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/builtinprograms.h>
#include <bmds/clock.h>
#include <bmds/directorywatch.h>
#include <bmds/fscommands.h>
#include <bmds/memorystream.h>
#include <bmds/partitions.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>
#include <bmds/thread.h>
#include <bmds/waitlist.h>
#include <bmds/semaphore.h>
#include "fakeconsole.h"
#include "hostmemory.h"
#include "renamechecks.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileInfo;
using bmds::FileType;
using bmds::FsFormat;
using bmds::PartitionInfo;
using bmds::RamBlockDevice;
using bmds::Stream;
using bmds::Timestamp;
using bmds::WaitForever;
using bmds::WritableByteSpan;
using bmds::BlockDevices::MaxName;
namespace Vfs = bmds::Vfs;

namespace {

        // A 64 MB RAM disk registered as ram0, set up once (the VFS's state is global).
        struct Disk {
                        Disk() : device(512) {
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(device.allocate(64 * 2048, bmds::MemSpace::Psram).isOk());
                                char name[MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(device, "ram", name).isOk());
                                REQUIRE(std::string(name) == "ram0");
                        }

                        RamBlockDevice device;
        };

        Disk &disk() {
                static Disk d;
                return d;
        }

        const Vfs::Context Root{"/", nullptr};

        // Live allocations but the RAM disk's chunks (its contents, not leaks).
        size_t liveObjects() {
                return HostMemory::liveAllocations() - disk().device.usedBytes() / RamBlockDevice::ChunkBytes;
        }

        // A fresh filesystem on the RAM disk, mounted at /ram0/p0 as the
        // storage service would.
        void fresh(FsFormat format = FsFormat::Fat32) {
                Disk &d = disk();
                Vfs::eject(d.device);
                d.device.setPresent(true);
                REQUIRE(Vfs::format(d.device, format).isOk());
                const bmds::Result<size_t> mounted = Vfs::automount(d.device);
                REQUIRE(mounted.error().isOk());
                REQUIRE(mounted.value() == 1);
        }

        FileHandle::Ptr open(const char *path, uint8_t flags, const Vfs::Context &context = Root) {
                auto [handle, err] = Vfs::open(path, flags, context);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                return handle;
        }

        void put(const char *path, const std::string &text, uint8_t flags = Vfs::Write | Vfs::Create | Vfs::Truncate) {
                FileHandle::Ptr h = open(path, flags);
                if (!text.empty())
                        REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                                         WaitForever)
                                        .error()
                                        .isOk());
        }

        std::string get(const char *path) {
                FileHandle::Ptr h = open(path, Vfs::Read);
                std::string     text;
                uint8_t         buf[700];
                for (;;) {
                        auto [n, err] = h->read(WritableByteSpan(buf), WaitForever);
                        if (err == Error::EndOfFile) break;
                        REQUIRE(err.isOk());
                        text.append(reinterpret_cast<char *>(buf), n);
                }
                return text;
        }

        std::vector<std::string> list(const char *path) {
                auto [dir, err] = Vfs::openDirectory(path, Root);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                std::vector<std::string> names;
                for (;;) {
                        auto [info, nextErr] = dir.next();
                        if (nextErr == Error::EndOfFile) break;
                        REQUIRE(nextErr.isOk());
                        names.push_back(std::string(info.name.cStr()) + (info.isDirectory() ? "/" : ""));
                }
                std::sort(names.begin(), names.end());
                return names;
        }

        std::string normalized(const char *path, const char *cwd) {
                auto [s, err] = Vfs::normalize(path, cwd);
                if (err.isError()) return err.name();
                return s.cStr();
        }

        std::vector<std::string> mountList() {
                Vfs::MountInfo           mounts[Vfs::MaxMounts];
                const size_t             n = Vfs::listMounts(bmds::Span<Vfs::MountInfo>(mounts));
                std::vector<std::string> out;
                for (size_t i = 0; i < n; ++i)
                        out.push_back(std::string(mounts[i].device) + " " + mounts[i].path + " " + mounts[i].type);
                return out;
        }

        void putLe32(uint8_t *p, uint32_t v) {
                for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        void putLe64(uint8_t *p, uint64_t v) {
                putLe32(p, static_cast<uint32_t>(v));
                putLe32(p + 4, static_cast<uint32_t>(v >> 32));
        }

} // namespace

TEST_CASE("Vfs: normalize") {
        CHECK(normalized("/", "/") == "/");
        CHECK(normalized("/sd0", "/") == "/sd0");
        CHECK(normalized("/sd0/", "/") == "/sd0");
        CHECK(normalized("//sd0//a///b/", "/") == "/sd0/a/b");
        CHECK(normalized("a/b", "/sd0/p0") == "/sd0/p0/a/b");
        CHECK(normalized("./a/./b/.", "/sd0") == "/sd0/a/b");
        CHECK(normalized("..", "/sd0/x") == "/sd0");
        CHECK(normalized("../../../..", "/sd0/x") == "/");
        CHECK(normalized("/sd0/a/../b", "/") == "/sd0/b");
        CHECK(normalized("..", "/") == "/");
        CHECK(normalized(".", "/sd0") == "/sd0");
        CHECK(normalized("", "/") == "Invalid");
        CHECK(normalized(std::string(1100, 'a').c_str(), "/") == "OutOfRange");
}

TEST_CASE("Partitions: reading tables") {
        RamBlockDevice dev(512);
        REQUIRE(dev.allocate(4096, bmds::MemSpace::Psram).isOk());
        PartitionInfo parts[8];
        auto          read = [&] {
                return bmds::Partitions::read(dev, bmds::Span<PartitionInfo>(parts));
        };
        uint8_t sector[512] = {};

        SUBCASE("a blank medium has none") {
                CHECK(read().value() == 0);
        }

        SUBCASE("MBR primary partitions, skipping empty, extended and out-of-range entries") {
                auto entry = [&](int slot, uint8_t type, uint32_t first, uint32_t count) {
                        uint8_t *e = sector + 446 + slot * 16;
                        e[4] = type;
                        putLe32(e + 8, first);
                        putLe32(e + 12, count);
                };
                entry(0, 0x0c, 2048, 1000);
                entry(1, 0x05, 100, 10);    // extended
                entry(2, 0x83, 3500, 2000); // past the end
                entry(3, 0x07, 3100, 900);
                sector[510] = 0x55;
                sector[511] = 0xaa;
                REQUIRE(dev.write(0, ByteSpan(sector, 512)).isOk());
                REQUIRE(read().value() == 2);
                CHECK(parts[0].firstSector == 2048);
                CHECK(parts[0].sectorCount == 1000);
                CHECK(parts[0].mbrType == 0x0c);
                CHECK(parts[1].firstSector == 3100);
                CHECK(parts[1].mbrType == 0x07);
        }

        SUBCASE("GPT, and a crafted header is refused") {
                sector[446 + 4] = 0xee; // protective MBR
                putLe32(sector + 446 + 8, 1);
                putLe32(sector + 446 + 12, 4095);
                sector[510] = 0x55;
                sector[511] = 0xaa;
                REQUIRE(dev.write(0, ByteSpan(sector, 512)).isOk());
                uint8_t header[512] = {};
                std::memcpy(header, "EFI PART", 8);
                putLe64(header + 72, 2);   // table at LBA 2
                putLe32(header + 80, 128); // entries
                putLe32(header + 84, 128); // entry size
                REQUIRE(dev.write(1, ByteSpan(header, 512)).isOk());
                uint8_t table[512] = {};
                table[0] = 1; // a type GUID
                putLe64(table + 32, 40);
                putLe64(table + 40, 1039);
                table[3 * 128] = 1; // the fourth entry; the two between are empty
                putLe64(table + 3 * 128 + 32, 2000);
                putLe64(table + 3 * 128 + 40, 4095);
                REQUIRE(dev.write(2, ByteSpan(table, 512)).isOk());
                REQUIRE(read().value() == 2);
                CHECK(parts[0].firstSector == 40);
                CHECK(parts[0].sectorCount == 1000);
                CHECK(parts[0].isGpt == 1);
                CHECK(parts[1].firstSector == 2000);
                CHECK(parts[1].sectorCount == 2096);

                putLe32(header + 80, 1000000); // a million entries
                REQUIRE(dev.write(1, ByteSpan(header, 512)).isOk());
                CHECK(read().value() == 0);
                putLe32(header + 80, 128);
                putLe64(header + 72, 4090); // the table runs off the end
                REQUIRE(dev.write(1, ByteSpan(header, 512)).isOk());
                CHECK(read().value() == 0);
        }

        SUBCASE("a whole-medium volume is one partition") {
                sector[0] = 0xeb;
                std::memcpy(sector + 3, "EXFAT   ", 8);
                sector[510] = 0x55;
                sector[511] = 0xaa;
                REQUIRE(dev.write(0, ByteSpan(sector, 512)).isOk());
                REQUIRE(read().value() == 1);
                CHECK(parts[0].firstSector == 0);
                CHECK(parts[0].sectorCount == 4096);
        }
}

TEST_CASE("Vfs: devices, partitions and mounts") {
        fresh(FsFormat::Fat32);
        bmds::BlockDevice &ram = disk().device;
        bmds::BlockDevice *p0 = bmds::BlockDevices::find("ram0p0");
        REQUIRE(p0 != nullptr);
        CHECK(Vfs::findDevice("/dev/ram0", Root) == &ram);
        CHECK(Vfs::findDevice("/dev/ram0p0", Root) == p0);
        CHECK(Vfs::findDevice("/dev/null", Root) == nullptr);
        CHECK(mountList() == std::vector<std::string>{"ram0p0 /ram0/p0 fat32"});

        CHECK(list("/") == std::vector<std::string>{"apps/", "bin/", "dev/", "etc/", "ram0/", "share/", "trash/"});
        CHECK(list("/ram0") == std::vector<std::string>{"p0/"});
        CHECK(list("/dev") == std::vector<std::string>{"clip0", "clip0p", "console", "null", "ram0", "ram0p0", "random",
                                                       "tty", "urandom", "zero"});
        CHECK(Vfs::stat("/ram0", Root).value().isDirectory());
        CHECK(Vfs::stat("/dev/ram0", Root).value().type == FileType::Device);
        CHECK(Vfs::stat("/dev/ram0", Root).value().size == 64u * 1024 * 1024);
        CHECK(Vfs::open("/dev/ram0", Vfs::Read, Root).error() == Error::NotSupported);
        {
                auto [info, err] = Vfs::statfs("/ram0/p0", Root);
                REQUIRE(err.isOk());
                CHECK(std::string(info.type) == "FAT32");
                CHECK(info.totalBytes > 60u * 1000 * 1000);
                CHECK(info.freeBytes <= info.totalBytes);
        }

        SUBCASE("mounting by hand, and what is refused") {
                CHECK(Vfs::unmount("/ram0/p0").isOk());
                CHECK(Vfs::unmount("/ram0/p0") == Error::NotExist);
                CHECK(list("/") == std::vector<std::string>{"apps/", "bin/", "dev/", "etc/", "share/", "trash/"});
                CHECK(Vfs::stat("/ram0", Root).error() == Error::NotExist);
                CHECK(Vfs::mount("/data/main", *p0).isOk());
                CHECK(Vfs::stat("/data/../data/main/../main/.", Root).value().isDirectory());
                CHECK(list("/") ==
                      std::vector<std::string>{"apps/", "bin/", "data/", "dev/", "etc/", "share/", "trash/"});
                CHECK(Vfs::mount("/data/main", *p0) == Error::Exists);
                CHECK(Vfs::mount("/other", *p0) == Error::Busy);
                CHECK(Vfs::mount("/other", ram) == Error::Busy); // the whole device under a mounted partition
                CHECK(Vfs::mount("/data/main/inner", ram) == Error::Invalid);
                CHECK(Vfs::mount("/data", ram) == Error::Invalid);
                CHECK(Vfs::mount("/dev/x", ram) == Error::Invalid);
                CHECK(Vfs::mount("/trash", ram) == Error::Invalid);
                CHECK(Vfs::mount("/", ram) == Error::Invalid);
                CHECK(Vfs::mount(std::string("/" + std::string(70, 'x')).c_str(), ram) == Error::Invalid);
                CHECK(Vfs::format(ram, FsFormat::ExFat) == Error::Busy);
                CHECK(Vfs::format(*p0, FsFormat::ExFat) == Error::Busy);
                CHECK(Vfs::unmount("/data/main").isOk());
                // A device without a filesystem.
                bmds::RamBlockDevice blank(512);
                REQUIRE(blank.allocate(256, bmds::MemSpace::Psram).isOk());
                CHECK(Vfs::mount("/blank", blank) == Error::NoFilesystem);
                CHECK(mountList().empty());
        }

        SUBCASE("a whole device formatted and mounted at /boot, pinned; mounts announced") {
                // As /boot on the flash (devplan/storage.md §13): FatFs finds the partition.
                bmds::RamBlockDevice flash(512);
                REQUIRE(flash.allocate(8192, bmds::MemSpace::Psram).isOk()); // 4 MB
                CHECK(Vfs::mount("/boot", flash) == Error::NoFilesystem);
                CHECK(Vfs::format(flash, FsFormat::Auto).isOk());
                bmds::Semaphore       woken(1, 0);
                bmds::WaitList::Entry entry(Vfs::mountWaiters(), woken);
                REQUIRE(entry.isAdded());
                CHECK(!Vfs::isMounted("/boot"));
                CHECK(Vfs::mount("/boot", flash).isOk());
                CHECK(woken.acquire(1).isOk()); // the mount announced
                CHECK(Vfs::isMounted("/boot"));
                CHECK(!Vfs::isMounted("/boo"));
                CHECK(Vfs::pin("/boot").isOk());
                CHECK(Vfs::pin("/nothing") == Error::NotExist);
                CHECK(Vfs::unmount("/boot") == Error::Busy); // pinned
                CHECK(Vfs::isMounted("/boot"));
                CHECK(Vfs::unmountDevice(flash) == 1); // the device itself going still takes it
                CHECK(woken.acquire(1).isOk());        // the unmount announced
                CHECK(!Vfs::isMounted("/boot"));
        }

        SUBCASE("formatting a partition alone") {
                CHECK(Vfs::unmount("/ram0/p0").isOk());
                REQUIRE(Vfs::format(*p0, FsFormat::ExFat).isOk());
                CHECK(Vfs::mount("/ram0/p0", *p0).isOk());
                CHECK(mountList() == std::vector<std::string>{"ram0p0 /ram0/p0 exfat"});
        }

        SUBCASE("ejecting and reinserting") {
                put("/ram0/p0/kept.txt", "kept");
                FileHandle::Ptr reading = open("/ram0/p0/kept.txt", Vfs::Read);
                auto [dir, dirErr] = Vfs::openDirectory("/ram0/p0", Root);
                REQUIRE(dirErr.isOk());
                // Pulled out: the device is gone before the storage service ejects it.
                disk().device.setPresent(false);
                uint8_t buf[8];
                CHECK(reading->read(WritableByteSpan(buf), WaitForever).error() == Error::NoMedia);
                CHECK(Vfs::open("/ram0/p0/kept.txt", Vfs::Read, Root).error() == Error::NoMedia);
                Vfs::eject(ram);
                CHECK(mountList().empty());
                CHECK(Vfs::stat("/ram0/p0/kept.txt", Root).error() == Error::NotExist);
                CHECK(list("/dev") == std::vector<std::string>{"clip0", "clip0p", "console", "null", "ram0", "random",
                                                               "tty", "urandom", "zero"});
                CHECK(bmds::BlockDevices::find("ram0p0") == nullptr);
                // Back in.
                disk().device.setPresent(true);
                REQUIRE(Vfs::automount(ram).value() == 1);
                CHECK(reading->read(WritableByteSpan(buf), WaitForever).error() == Error::NoMedia);
                CHECK(dir.next().error() == Error::NoMedia);
                CHECK(get("/ram0/p0/kept.txt") == "kept");
                // A stale file doesn't hold FatFs's lock on the new mount.
                CHECK(Vfs::open("/ram0/p0/kept.txt", Vfs::Write, Root).error().isOk());
        }

        SUBCASE("formats") {
                Vfs::eject(ram);
                REQUIRE(Vfs::format(ram, FsFormat::ExFat).isOk());
                REQUIRE(Vfs::automount(ram).value() == 1);
                CHECK(mountList() == std::vector<std::string>{"ram0p0 /ram0/p0 exfat"});
                Vfs::eject(ram);
                REQUIRE(Vfs::format(ram, FsFormat::Auto).isOk());
                REQUIRE(Vfs::automount(ram).value() == 1);
                CHECK(mountList() == std::vector<std::string>{"ram0p0 /ram0/p0 fat16"}); // FatFs's choice for 64 MB
        }
}

namespace {

        int testProgram(const bmds::BuiltinProgram::Context &) {
                return 7;
        }

        // A registry for the tests, sorted by name (as the linker sorts the firmware's).
        constexpr bmds::ProgramEntry TestPrograms[] = {
                {"bash", {testProgram, 4096}},
                {"sh", {testProgram, 4096}},
                {"term", {testProgram, 8192}},
        };

} // namespace

TEST_CASE("Vfs: /bin lists the programs built in, read-only") {
        bmds::BuiltinPrograms::detail::setForHost(bmds::Span<const bmds::ProgramEntry>(TestPrograms));
        CHECK(bmds::BuiltinPrograms::find("sh") == &TestPrograms[1]);
        CHECK(bmds::BuiltinPrograms::find("term") == &TestPrograms[2]);
        CHECK(bmds::BuiltinPrograms::find("ls") == nullptr);
        fresh(FsFormat::Fat32);
        CHECK(list("/bin") == std::vector<std::string>{"bash", "sh", "term"});
        CHECK(Vfs::stat("/bin", Root).value().isDirectory());
        {
                const auto info = Vfs::stat("/bin/sh", Root);
                REQUIRE(info.isOk());
                CHECK(info.value().type == FileType::File);
                CHECK(info.value().size == 0);
        }
        CHECK(Vfs::stat("/bin/ls", Root).error() == Error::NotExist);
        CHECK(Vfs::stat("/bin/sh/x", Root).error() == Error::NotExist);
        // Read: empty. Written, made, removed, renamed: refused.
        {
                auto [file, err] = Vfs::open("/bin/sh", Vfs::Read, Root);
                REQUIRE(err.isOk());
                uint8_t    buf[4];
                const auto got = file->read(bmds::WritableByteSpan(buf, sizeof(buf)), 0);
                CHECK((got.isError() || got.value() == 0));
        }
        CHECK(Vfs::open("/bin/sh", Vfs::Write, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/bin/new", Vfs::Write | Vfs::Create, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/bin", Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::mkdir("/bin/dir", Root) == Error::ReadOnly);
        CHECK(Vfs::remove("/bin/sh", Root) == Error::ReadOnly);
        CHECK(Vfs::rmdir("/bin", Root) == Error::ReadOnly);
        CHECK(Vfs::rename("/bin/sh", "/bin/zsh", Vfs::RenameMode::NoReplace, Root) == Error::ReadOnly);
        CHECK(Vfs::rename("/ram0/p0", "/bin/x", Vfs::RenameMode::NoReplace, Root) == Error::ReadOnly);
        CHECK(Vfs::symlink("/bin/sh", "/bin/link", Root) == Error::ReadOnly);
        CHECK(Vfs::openDirectory("/bin/sh", Root).error() == Error::NotDir);
        bmds::BlockDevice *p0 = bmds::BlockDevices::find("ram0p0");
        REQUIRE(p0 != nullptr);
        CHECK(Vfs::mount("/bin", *p0) == Error::Invalid);
        // What a path runs: a built-in by its path, or through a link to it.
        CHECK(Vfs::builtinProgram("/bin/sh", Root) == &TestPrograms[1]);
        CHECK(Vfs::builtinProgram("/bin/../bin/term", Root) == &TestPrograms[2]);
        CHECK(Vfs::builtinProgram("/bin/ls", Root) == nullptr);
        CHECK(Vfs::symlink("/bin/sh", "/ram0/p0/mysh", Root).isOk());
        CHECK(Vfs::builtinProgram("/ram0/p0/mysh", Root) == &TestPrograms[1]);
        CHECK(Vfs::builtinProgram("/ram0/p0", Root) == nullptr);
        // resolve(): the same, with the path followed to it.
        {
                auto [found, err] = Vfs::resolve("/ram0/p0/mysh", Root);
                REQUIRE(err.isOk());
                CHECK(found.program == &TestPrograms[1]);
                CHECK(found.path == "/bin/sh");
        }
        CHECK(Vfs::resolve("/ram0/p0", Root).value().program == nullptr);
        CHECK(Vfs::resolve("/bin/ls", Root).error() == Error::NotExist);
        bmds::BuiltinPrograms::detail::setForHost(bmds::Span<const bmds::ProgramEntry>());
        CHECK(list("/bin").empty());
}

TEST_CASE("Vfs: files") {
        for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                CAPTURE(static_cast<int>(format));
                fresh(format);
                const size_t base = liveObjects();

                put("/ram0/p0/hello.txt", "hello, world\n");
                CHECK(get("/ram0/p0/hello.txt") == "hello, world\n");
                CHECK(Vfs::open("/RAM0/p0/hello.txt", Vfs::Read, Root).error() ==
                      Error::NotExist);                               // mount paths are exact
                CHECK(get("/ram0/p0/HELLO.TXT") == "hello, world\n"); // FAT names are not

                {
                        auto [info, err] = Vfs::stat("/ram0/p0/hello.txt", Root);
                        REQUIRE(err.isOk());
                        CHECK(info.name == "hello.txt");
                        CHECK(info.type == FileType::File);
                        CHECK(info.size == 13);
                }

                // Overwrite, append, truncate.
                put("/ram0/p0/hello.txt", "bye");
                CHECK(get("/ram0/p0/hello.txt") == "bye");
                put("/ram0/p0/hello.txt", " now", Vfs::Write | Vfs::Append);
                CHECK(get("/ram0/p0/hello.txt") == "bye now");
                put("/ram0/p0/hello.txt", "", Vfs::Write | Vfs::Truncate);
                CHECK(get("/ram0/p0/hello.txt") == "");

                // A file bigger than a cluster, written and read in odd pieces.
                std::string big;
                for (int i = 0; i < 20000; ++i) big += static_cast<char>('a' + i * 7 % 26);
                put("/ram0/p0/big.bin", big);
                CHECK(get("/ram0/p0/big.bin") == big);

                // Seeking.
                {
                        FileHandle::Ptr h = open("/ram0/p0/big.bin", Vfs::Read | Vfs::Write);
                        Stream         &s = h->stream();
                        CHECK(s.seek(0, Stream::SeekFrom::End).value() == 20000);
                        CHECK(s.seek(-10, Stream::SeekFrom::Current).value() == 19990);
                        CHECK(s.seek(-1, Stream::SeekFrom::Start).error() == Error::Invalid);
                        CHECK(s.seek(5, Stream::SeekFrom::Start).value() == 5);
                        CHECK(h->write(bmds::StringView("XY"), WaitForever).error().isOk());
                        uint8_t c = 0;
                        CHECK(h->read(WritableByteSpan(&c, 1), WaitForever).value() == 1);
                        CHECK(c == static_cast<uint8_t>(big[7]));
                        // Past the end: the file grows when written there.
                        CHECK(s.seek(20010, Stream::SeekFrom::Start).value() == 20010);
                        CHECK(h->write(bmds::StringView("!"), WaitForever).error().isOk());
                        CHECK(s.flush(WaitForever).isOk());
                }
                CHECK(Vfs::stat("/ram0/p0/big.bin", Root).value().size == 20011);
                CHECK(get("/ram0/p0/big.bin").substr(5, 2) == "XY");

                // Handles share one offset (dup / 2>&1).
                {
                        FileHandle::Ptr a = open("/ram0/p0/shared.txt", Vfs::Write | Vfs::Create | Vfs::Truncate);
                        FileHandle::Ptr b = a;
                        CHECK(a->write(bmds::StringView("one "), WaitForever).error().isOk());
                        CHECK(b->write(bmds::StringView("two"), WaitForever).error().isOk());
                }
                CHECK(get("/ram0/p0/shared.txt") == "one two");

                // Read-only handles.
                {
                        FileHandle::Ptr r = open("/ram0/p0/shared.txt", Vfs::Read);
                        CHECK(r->write(bmds::StringView("x"), WaitForever).error() == Error::BadDescriptor);
                }
                CHECK(liveObjects() == base);
        }
}

TEST_CASE("Vfs: directories and names") {
        fresh(FsFormat::ExFat);
        const size_t base = liveObjects();

        CHECK(Vfs::mkdir("/ram0/p0/docs", Root).isOk());
        CHECK(Vfs::mkdir("/ram0/p0/docs/deeper", Root).isOk());
        const Vfs::Context inDocs{"/ram0/p0/docs", nullptr};
        put("/ram0/p0/docs/a.txt", "A");
        CHECK(Vfs::mkdir("deeper/deepest", inDocs).isOk());
        const char *longName = "/ram0/p0/docs/Grüße aus Zürich — ein recht langer Dateiname ✓ 日本.txt";
        put(longName, "unicode");
        CHECK(get(longName) == "unicode");

        CHECK(list("/ram0/p0") == std::vector<std::string>{"docs/"});
        CHECK(list("/ram0/p0/docs") ==
              std::vector<std::string>{"Grüße aus Zürich — ein recht langer Dateiname ✓ 日本.txt", "a.txt", "deeper/"});
        CHECK(list("/ram0/p0/docs/./deeper/..") == list("/ram0/p0/docs"));
        {
                auto [info, err] = Vfs::stat("..", inDocs);
                REQUIRE(err.isOk());
                CHECK(info.name == "p0");
                CHECK(info.isDirectory());
                CHECK(Vfs::stat("deeper", inDocs).value().isDirectory());
        }

        SUBCASE("rename and move") {
                CHECK(Vfs::rename("/ram0/p0/docs/a.txt", "/ram0/p0/docs/b.txt", Vfs::RenameMode::NoReplace, Root)
                              .isOk());
                CHECK(Vfs::rename("b.txt", "deeper/c.txt", Vfs::RenameMode::NoReplace, inDocs).isOk());
                CHECK(get("/ram0/p0/docs/deeper/c.txt") == "A");
                CHECK(Vfs::stat("/ram0/p0/docs/a.txt", Root).error() == Error::NotExist);
                put("/ram0/p0/docs/d.txt", "D");
                CHECK(Vfs::rename("/ram0/p0/docs/d.txt", "/ram0/p0/docs/deeper/c.txt", Vfs::RenameMode::NoReplace,
                                  Root) == Error::Exists);
                CHECK(Vfs::rename("/ram0/p0/docs", "/ram0/p0/papers", Vfs::RenameMode::NoReplace, Root).isOk());
                CHECK(get("/ram0/p0/papers/deeper/c.txt") == "A");
                CHECK(Vfs::rename("/ram0/p0/papers/d.txt", "/dev/d.txt", Vfs::RenameMode::NoReplace, Root) ==
                      Error::NotSupported);
                CHECK(Vfs::rename("/ram0/p0/papers/d.txt", "/elsewhere/d.txt", Vfs::RenameMode::NoReplace, Root) ==
                      Error::NotSupported);
        }

        SUBCASE("remove and rmdir") {
                CHECK(Vfs::rmdir("/ram0/p0/docs/deeper", Root) == Error::NotEmpty);
                CHECK(Vfs::rmdir("/ram0/p0/docs/a.txt", Root) == Error::NotDir);
                CHECK(Vfs::remove("/ram0/p0/docs/deeper", Root) == Error::IsDir);
                CHECK(Vfs::rmdir("/ram0/p0/docs/deeper/deepest", Root).isOk());
                CHECK(Vfs::rmdir("/ram0/p0/docs/deeper", Root).isOk());
                CHECK(Vfs::remove("/ram0/p0/docs/a.txt", Root).isOk());
                CHECK(Vfs::remove("/ram0/p0/docs/a.txt", Root) == Error::NotExist);
                CHECK(Vfs::rmdir("/ram0/p0", Root) == Error::Busy);
                CHECK(Vfs::rmdir("/ram0", Root) == Error::PermissionDenied);
                CHECK(Vfs::remove("/ram0/p0", Root) == Error::IsDir);
                CHECK(Vfs::remove("/dev/null", Root) == Error::PermissionDenied);
        }

        SUBCASE("errors") {
                CHECK(Vfs::open("/ram0/p0/missing", Vfs::Read, Root).error() == Error::NotExist);
                CHECK(Vfs::open("/ram0/p0/nodir/x", Vfs::Write | Vfs::Create, Root).error() == Error::NotExist);
                CHECK(Vfs::open("/ram0/p0/docs", Vfs::Read, Root).error() == Error::IsDir);
                CHECK(Vfs::open("/ram0/p0/docs", Vfs::Write | Vfs::Create, Root).error() == Error::IsDir);
                CHECK(Vfs::open("/ram0/p0", Vfs::Read, Root).error() == Error::IsDir);
                CHECK(Vfs::open("/ram0", Vfs::Read, Root).error() == Error::IsDir);
                CHECK(Vfs::open("/ram0/p0/docs/a.txt", 0, Root).error() == Error::Invalid);
                CHECK(Vfs::open("/ram0/p0/docs/a.txt", Vfs::Read | Vfs::Truncate, Root).error() == Error::Invalid);
                CHECK(Vfs::open("/ram0/p0/docs/a.txt", Vfs::Write | Vfs::Create | Vfs::Exclusive, Root).error() ==
                      Error::Exists);
                CHECK(Vfs::open("/nowhere/x", Vfs::Read, Root).error() == Error::NotExist);
                CHECK(Vfs::openDirectory("/ram0/p0/docs/a.txt", Root).error() == Error::NotDir);
                CHECK(Vfs::openDirectory("/ram0/p0/missing", Root).error() == Error::NotExist);
                CHECK(Vfs::mkdir("/ram0/p0/docs", Root) == Error::Exists);
                CHECK(Vfs::mkdir("/ram0/p0", Root) == Error::Exists);
                CHECK(Vfs::mkdir("/ram0", Root) == Error::Exists);
                CHECK(Vfs::mkdir("/mine", Root) == Error::PermissionDenied);
                CHECK(Vfs::mkdir("/ram0/p9", Root) == Error::PermissionDenied);
                CHECK(Vfs::mkdir("/ram0/p0/a/b/c", Root) == Error::NotExist);
                CHECK(Vfs::open("/ram0/p0/bad<name>", Vfs::Write | Vfs::Create, Root).error() == Error::Invalid);
                // FatFs's lock: one writer per file, no removing open files.
                {
                        FileHandle::Ptr w = open("/ram0/p0/docs/a.txt", Vfs::Write);
                        CHECK(Vfs::open("/ram0/p0/docs/a.txt", Vfs::Write, Root).error() == Error::Busy);
                        CHECK(Vfs::remove("/ram0/p0/docs/a.txt", Root) == Error::Busy);
                }
                CHECK(Vfs::open("/ram0/p0/docs/a.txt", Vfs::Write, Root).error().isOk());
                // At most 16 open files and directories.
                {
                        std::vector<FileHandle::Ptr> handles;
                        Error                        last;
                        for (int i = 0; i < 20 && last.isOk(); ++i) {
                                const std::string path = "/ram0/p0/f" + std::to_string(i);
                                auto [h, err] = Vfs::open(path.c_str(), Vfs::Write | Vfs::Create, Root);
                                last = err;
                                if (err.isOk()) handles.push_back(std::move(h));
                        }
                        CHECK(last == Error::TooManyOpen);
                        CHECK(handles.size() == 16);
                }
        }
        CHECK(liveObjects() == base);
}

TEST_CASE("Vfs: holes and truncate") {
        for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                CAPTURE(static_cast<int>(format));
                fresh(format);
                const size_t base = liveObjects();
                {
                        FileHandle::Ptr h = open("/ram0/p0/h.bin", Vfs::Read | Vfs::Write | Vfs::Create);
                        Stream         &s = h->stream();
                        CHECK(h->write(bmds::StringView("abc"), WaitForever).error().isOk());
                        // Seeking past the end doesn't grow the file; reading there is the end.
                        CHECK(s.seek(5000, Stream::SeekFrom::Start).value() == 5000);
                        CHECK(s.seek(0, Stream::SeekFrom::Current).value() == 5000);
                        uint8_t c = 1;
                        CHECK(h->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::EndOfFile);
                        CHECK(s.seek(0, Stream::SeekFrom::End).value() == 3);
                        CHECK(s.seek(5000, Stream::SeekFrom::Start).value() == 5000);
                        // Writing there fills the gap with zeros.
                        CHECK(h->write(bmds::StringView("Z"), WaitForever).error().isOk());
                        CHECK(s.flush(WaitForever).isOk());
                        // Cut, with the position kept past the new end; grown with zeros.
                        CHECK(s.truncate(2).isOk());
                        CHECK(s.seek(0, Stream::SeekFrom::Current).value() == 5001);
                        CHECK(s.seek(0, Stream::SeekFrom::End).value() == 2);
                        CHECK(s.truncate(10).isOk());
                        CHECK(s.seek(0, Stream::SeekFrom::End).value() == 10);
                }
                // (FatFs refuses a second open while a file is open for writing.)
                CHECK(get("/ram0/p0/h.bin") == std::string("ab") + std::string(8, '\0'));
                put("/ram0/p0/g.bin", "x");
                {
                        FileHandle::Ptr h = open("/ram0/p0/g.bin", Vfs::Write);
                        CHECK(h->stream().seek(3000, Stream::SeekFrom::Start).isOk());
                        CHECK(h->write(bmds::StringView("y"), WaitForever).error().isOk());
                }
                const std::string g = get("/ram0/p0/g.bin");
                CHECK(g.size() == 3001);
                CHECK(g.front() == 'x');
                CHECK(g.back() == 'y');
                CHECK(g.find_first_not_of('\0', 1) == 3000); // zeros between, not old clusters
                // Only files open for writing are cut.
                {
                        FileHandle::Ptr r = open("/ram0/p0/g.bin", Vfs::Read);
                        CHECK(r->stream().truncate(0) == Error::BadDescriptor);
                }
                CHECK(liveObjects() == base);
        }
}

TEST_CASE("Vfs: file identity") {
        fresh(FsFormat::Fat32);
        CHECK(Vfs::mkdir("/ram0/p0/Dir", Root).isOk());
        put("/ram0/p0/Dir/File.txt", "x");
        const FileInfo a = Vfs::stat("/ram0/p0/Dir/File.txt", Root).value();
        CHECK(a.id != 0);
        CHECK(a.device != 0);
        // The same for every name of it (case within the volume, ., ..) and its handles; not for others.
        CHECK(Vfs::stat("/ram0/p0/dir/../Dir/./FILE.TXT", Root).value().id == a.id);
        CHECK(Vfs::stat("File.txt", Vfs::Context{"/ram0/p0/Dir", nullptr}).value().id == a.id);
        CHECK(open("/ram0/p0/dir/file.txt", Vfs::Read)->id() == a.id);
        CHECK(open("/ram0/p0/dir/file.txt", Vfs::Read)->device() == a.device);
        CHECK(Vfs::stat("/ram0/p0/Dir", Root).value().id != a.id);
        CHECK(Vfs::stat("/ram0/p0", Root).value().id != Vfs::stat("/", Root).value().id);
        auto [dir, err] = Vfs::openDirectory("/ram0/p0/dir", Root);
        REQUIRE(err.isOk());
        const FileInfo listed = dir.next().value();
        CHECK(listed.id == a.id);
        CHECK(listed.device == a.device);
        // Devices: their stream, as their handles say.
        const uint64_t null = Vfs::stat("/dev/null", Root).value().id;
        CHECK(null == open("/dev/null", Vfs::Write)->id());
        CHECK(Vfs::stat("/dev/null", Root).value().device == 0);
}

namespace {

        // What the change hook was told, in order.
        std::vector<std::pair<uint32_t, uint64_t>> changes;

        void recordChange(uint32_t device, uint64_t id) {
                changes.emplace_back(device, id);
                return;
        }

} // namespace

TEST_CASE("Vfs: the change hook") {
        fresh(FsFormat::Fat32);
        put("/ram0/p0/prog", "one");
        CHECK(Vfs::symlink("prog", "/ram0/p0/link", Root).isOk());
        const FileInfo prog = Vfs::stat("/ram0/p0/prog", Root).value();
        const auto     told = std::make_pair(prog.device, prog.id);
        Vfs::setChangeHook(recordChange);
        changes.clear();
        // Reading tells nothing; a write handle's close does (through a link: the file's id).
        CHECK(get("/ram0/p0/link") == "one");
        CHECK(changes.empty());
        {
                FileHandle::Ptr h = open("/ram0/p0/link", Vfs::Write | Vfs::Truncate);
                FileHandle::Ptr d = h; // the last handle's close, once
                CHECK(changes.empty());
        }
        REQUIRE(changes.size() == 1);
        CHECK(changes[0] == told);
        changes.clear();
        CHECK(Vfs::setModified("/ram0/p0/link", bmds::Timestamp::fromSeconds(1700000000), Root).isOk());
        CHECK(Vfs::rename("/ram0/p0/prog", "/ram0/p0/moved", Vfs::RenameMode::NoReplace, Root).isOk());
        const FileInfo moved = Vfs::stat("/ram0/p0/moved", Root).value();
        CHECK(Vfs::remove("/ram0/p0/moved", Root).isOk());
        REQUIRE(changes.size() == 3);
        CHECK(changes[0] == told); // touched (through the link)
        CHECK(changes[1] == told); // renamed: its old name
        CHECK(changes[2] == std::make_pair(moved.device, moved.id));
        CHECK(moved.id != prog.id);
        changes.clear();
        // Failures tell nothing; an unmount tells everything.
        CHECK(Vfs::remove("/ram0/p0/moved", Root) == Error::NotExist);
        CHECK(changes.empty());
        CHECK(Vfs::unmount("/ram0/p0").isOk());
        REQUIRE(changes.size() == 1);
        CHECK(changes[0] == std::make_pair(uint32_t(0), uint64_t(0)));
        Vfs::setChangeHook(nullptr);
        changes.clear();
        fresh(FsFormat::Fat32);
}

namespace {

        // A watch that counts its notifies.
        struct Watched {
                        Watched() { watch.setNotify(count, this); }

                        static void count(void *arg) {
                                ++static_cast<Watched *>(arg)->told;
                                return;
                        }

                        // The changes since the last look, and that a notify came for them.
                        uint8_t take() {
                                const uint8_t taken = watch.takeChanges();
                                CHECK((taken == 0) == (told == 0));
                                told = 0;
                                return taken;
                        }

                        bmds::DirectoryWatch watch;
                        int                  told = 0;
        };

        using Change = bmds::DirectoryWatch::Change;

} // namespace

TEST_CASE("Vfs: /apps, links to applications' programs, read-only") {
        fresh(FsFormat::Fat32);
        CHECK(list("/apps").empty());
        CHECK(Vfs::stat("/apps", Root).value().isDirectory());
        CHECK(Vfs::mkdir("/ram0/p0/apps", Root).isOk());
        CHECK(Vfs::mkdir("/ram0/p0/apps/blocks.app", Root).isOk());
        put("/ram0/p0/apps/blocks.app/blocks", "program");
        Watched apps;
        CHECK(apps.watch.watch("/apps", Root).isOk());
        const Vfs::AppLink links[] = {{"blocks", "/ram0/p0/apps/blocks.app/blocks"},
                                      {"tour", "/ram0/p0/apps/tour.app"}};
        CHECK(Vfs::setAppLinks(bmds::Span<const Vfs::AppLink>(links, 2)).isOk());
        CHECK(apps.take() == Change::Entries);
        CHECK(list("/apps") == std::vector<std::string>{"blocks", "tour"});
        // The link itself, and where it leads.
        CHECK(Vfs::lstat("/apps/blocks", Root).value().type == FileType::Link);
        CHECK(Vfs::readLink("/apps/blocks", Root).value().view() == "/ram0/p0/apps/blocks.app/blocks");
        CHECK(Vfs::stat("/apps/blocks", Root).value().type == FileType::File);
        CHECK(get("/apps/blocks") == "program");
        CHECK(Vfs::realPath("/apps/blocks", Root).value().view() == "/ram0/p0/apps/blocks.app/blocks");
        CHECK(Vfs::stat("/apps/tour", Root).error() == Error::NotExist); // leads nowhere
        CHECK(Vfs::stat("/apps/other", Root).error() == Error::NotExist);
        // Read-only: nothing made, removed or renamed; no mount there.
        CHECK(Vfs::open("/apps/new", Vfs::Write | Vfs::Create, Root).error() == Error::ReadOnly);
        CHECK(Vfs::remove("/apps/blocks", Root) == Error::ReadOnly);
        CHECK(Vfs::mkdir("/apps/dir", Root) == Error::ReadOnly);
        CHECK(Vfs::symlink("/x", "/apps/x", Root) == Error::ReadOnly);
        bmds::BlockDevice *p0 = bmds::BlockDevices::find("ram0p0");
        REQUIRE(p0 != nullptr);
        CHECK(Vfs::mount("/apps", *p0) == Error::Invalid);
        // Refused sets change nothing; an empty one empties it.
        const Vfs::AppLink bad[] = {{"a/b", "/x"}};
        CHECK(Vfs::setAppLinks(bmds::Span<const Vfs::AppLink>(bad, 1)) == Error::Invalid);
        const Vfs::AppLink relative[] = {{"a", "x"}};
        CHECK(Vfs::setAppLinks(bmds::Span<const Vfs::AppLink>(relative, 1)) == Error::Invalid);
        CHECK(list("/apps").size() == 2);
        CHECK(Vfs::setAppLinks(bmds::Span<const Vfs::AppLink>()).isOk());
        CHECK(list("/apps").empty());
        CHECK(apps.take() == Change::Entries);
}

TEST_CASE("Vfs: directory watches") {
        fresh(FsFormat::Fat32);
        CHECK(Vfs::mkdir("/ram0/p0/dir", Root).isOk());
        CHECK(Vfs::mkdir("/ram0/p0/other", Root).isOk());
        CHECK(Vfs::symlink("dir", "/ram0/p0/link", Root).isOk());
        put("/ram0/p0/dir/old", "old");
        const size_t base = liveObjects();
        Watched      dir, other, root, sub;
        // Through a link and in another case: the folder it is.
        CHECK(dir.watch.watch("/ram0/p0/LINK", Root).isOk());
        CHECK(other.watch.watch("/ram0/p0/other", Root).isOk());
        CHECK(root.watch.watch("/ram0/p0", Root).isOk());
        CHECK(dir.watch.isWatching());
        CHECK(liveObjects() == base); // nothing allocated
        // Reading tells nothing.
        CHECK(get("/ram0/p0/dir/old") == "old");
        CHECK(list("/ram0/p0/dir") == std::vector<std::string>{"old"});
        CHECK(dir.take() == 0);
        // A file made: told when it closes after writing; the other folders aren't.
        {
                FileHandle::Ptr h = open("/ram0/p0/dir/new", Vfs::Write | Vfs::Create);
                CHECK(dir.take() == 0);
        }
        CHECK(dir.take() == Change::Contents);
        CHECK(other.take() == 0);
        CHECK(root.take() == 0);
        // Changes accumulate until taken, with a notify each.
        put("/ram0/p0/link/new", "written");
        CHECK(Vfs::setModified("/ram0/p0/dir/old", Timestamp::fromSeconds(1700000000), Root).isOk());
        CHECK(Vfs::mkdir("/ram0/p0/dir/sub", Root).isOk());
        CHECK(dir.told == 3);
        CHECK(dir.take() == (Change::Contents | Change::Entries));
        // Removed, made, renamed: Entries; a move tells both folders.
        CHECK(Vfs::remove("/ram0/p0/dir/new", Root).isOk());
        CHECK(dir.take() == Change::Entries);
        CHECK(Vfs::symlink("old", "/ram0/p0/dir/ln", Root).isOk());
        CHECK(dir.take() == Change::Entries);
        CHECK(Vfs::rename("/ram0/p0/dir/old", "/ram0/p0/other/old", Vfs::RenameMode::NoReplace, Root).isOk());
        CHECK(dir.take() == Change::Entries);
        CHECK(other.take() == Change::Entries);
        CHECK(root.take() == 0);
        // A watched folder renamed or removed is told itself, as its folder is.
        CHECK(sub.watch.watch("/ram0/p0/dir/sub", Root).isOk());
        CHECK(Vfs::rename("/ram0/p0/dir/sub", "/ram0/p0/dir/sub2", Vfs::RenameMode::NoReplace, Root).isOk());
        CHECK(sub.take() == Change::Entries);
        CHECK(dir.take() == Change::Entries);
        CHECK(sub.watch.watch("/ram0/p0/dir/sub2", Root).isOk());
        CHECK(Vfs::rmdir("/ram0/p0/dir/sub2", Root).isOk());
        CHECK(sub.take() == Change::Entries);
        CHECK(dir.take() == Change::Entries);
        // Failures tell nothing.
        CHECK(Vfs::remove("/ram0/p0/dir/none", Root) == Error::NotExist);
        CHECK(Vfs::mkdir("/ram0/p0/dir/ln", Root) == Error::Exists);
        CHECK(dir.take() == 0);
        // Moved to another folder; stopped: told no more.
        CHECK(dir.watch.watch("/ram0/p0/other", Root).isOk());
        CHECK(Vfs::remove("/ram0/p0/dir/ln", Root).isOk());
        CHECK(dir.take() == 0);
        put("/ram0/p0/other/x", "x");
        CHECK(dir.take() == Change::Contents);
        CHECK(other.take() == Change::Contents);
        other.watch.stop();
        CHECK(!other.watch.isWatching());
        put("/ram0/p0/other/x", "y");
        CHECK(other.take() == 0);
        CHECK(dir.take() == Change::Contents); // still watching it
        // Not a folder, or not there: listed with no folder, told of mounts only.
        CHECK(sub.watch.watch("/ram0/p0/other/x", Root) == Error::NotDir);
        CHECK(other.watch.watch("/ram0/p0/later", Root) == Error::NotExist);
        CHECK(other.watch.isWatching());
        CHECK(Vfs::mkdir("/ram0/p0/later", Root).isOk());
        CHECK(other.take() == 0);
        CHECK(root.take() == Change::Entries);
        CHECK(Vfs::unmount("/ram0/p0").isOk());
        for (Watched *w : {&dir, &other, &root, &sub}) CHECK(w->take() == Change::Mounts);
        fresh(FsFormat::Fat32);
        for (Watched *w : {&dir, &other, &root, &sub}) CHECK(w->take() == Change::Mounts);
        CHECK(other.watch.watch("/ram0/p0/later", Root) == Error::NotExist); // a fresh filesystem
        CHECK(other.watch.watch("/ram0/p0", Root).isOk());
        put("/ram0/p0/again", "x");
        CHECK(other.take() == Change::Contents);
        CHECK(root.take() == Change::Contents);
}

TEST_CASE("Vfs: FAT lookups kept between changes, and not past one") {
        // devplan/storage.md §3: FatVolume keeps f_stat()'s results until the volume changes.
        for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                CAPTURE(static_cast<int>(format));
                fresh(format);
                const size_t base = liveObjects();
                const char  *path = "/ram0/p0/kept.txt";
                // Missing, kept as missing; then made: seen at once.
                CHECK(Vfs::stat(path, Root).error() == Error::NotExist);
                CHECK(Vfs::stat(path, Root).error() == Error::NotExist);
                put(path, "abc");
                CHECK(Vfs::stat(path, Root).value().size == 3);
                CHECK(Vfs::stat(path, Root).value().size == 3);
                // Written through a handle still open: the size after a flush, as FatFs gives it.
                {
                        FileHandle::Ptr h = open(path, Vfs::Write | Vfs::Append);
                        CHECK(h->write(bmds::StringView("defgh"), WaitForever).isOk());
                        CHECK(h->stream().flush(WaitForever).isOk());
                        CHECK(Vfs::stat(path, Root).value().size == 8);
                        CHECK(h->stream().truncate(2).isOk());
                        CHECK(h->stream().flush(WaitForever).isOk());
                        CHECK(Vfs::stat(path, Root).value().size == 2);
                }
                CHECK(Vfs::stat(path, Root).value().size == 2);
                // Truncated by an open, touched, renamed, removed.
                put(path, "");
                CHECK(Vfs::stat(path, Root).value().size == 0);
                CHECK(Vfs::setModified(path, bmds::Timestamp::fromSeconds(1700000000), Root).isOk());
                CHECK(Vfs::stat(path, Root).value().modified.seconds() == 1700000000);
                CHECK(Vfs::rename(path, "/ram0/p0/moved.txt", Vfs::RenameMode::NoReplace, Root).isOk());
                CHECK(Vfs::stat(path, Root).error() == Error::NotExist);
                CHECK(Vfs::stat("/ram0/p0/moved.txt", Root).isOk());
                CHECK(Vfs::remove("/ram0/p0/moved.txt", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/moved.txt", Root).error() == Error::NotExist);
                // A directory made and removed; a path through a missing one.
                CHECK(Vfs::stat("/ram0/p0/d/x", Root).error() == Error::NotExist);
                CHECK(Vfs::mkdir("/ram0/p0/d", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/d", Root).value().isDirectory());
                put("/ram0/p0/d/x", "x");
                CHECK(Vfs::stat("/ram0/p0/d/x", Root).value().size == 1);
                CHECK(Vfs::remove("/ram0/p0/d/x", Root).isOk());
                CHECK(Vfs::rmdir("/ram0/p0/d", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/d", Root).error() == Error::NotExist);
                // A link written where a missing name was kept.
                CHECK(Vfs::stat("/ram0/p0/l", Root).error() == Error::NotExist);
                put("/ram0/p0/target", "t");
                CHECK(Vfs::symlink("target", "/ram0/p0/l", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/l", Root).value().size == 1);
                CHECK(liveObjects() == base);
        }
}

TEST_CASE("Vfs: symbolic links") {
        for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                CAPTURE(static_cast<int>(format));
                fresh(format);
                const size_t base = liveObjects();
                CHECK(Vfs::mkdir("/ram0/p0/bin", Root).isOk());
                CHECK(Vfs::mkdir("/ram0/p0/data", Root).isOk());
                put("/ram0/p0/bin/busybox", "BB");
                put("/ram0/p0/data/notes.txt", "notes");
                // Relative to the link's directory, and absolute.
                CHECK(Vfs::symlink("busybox", "/ram0/p0/bin/awk", Root).isOk());
                CHECK(Vfs::symlink("/ram0/p0/data", "/ram0/p0/d", Root).isOk());
                CHECK(Vfs::symlink("busybox", "/ram0/p0/bin/awk", Root) == Error::Exists);
                CHECK(Vfs::symlink("", "/ram0/p0/x", Root) == Error::Invalid);
                CHECK(Vfs::symlink("x", "/dev/x", Root) == Error::PermissionDenied);

                // On disk: Cygwin's format, with the System attribute.
                {
                        const std::string raw = get("/ram0/p0/bin/awk"); // followed: the target's contents
                        CHECK(raw == "BB");
                }
                const FileInfo awk = Vfs::lstat("/ram0/p0/bin/awk", Root).value();
                CHECK(awk.type == FileType::Link);
                CHECK(awk.size == 7);
                CHECK((awk.attributes & FileInfo::System) != 0);
                CHECK(Vfs::readLink("/ram0/p0/bin/awk", Root).value() == "busybox");
                CHECK(Vfs::readLink("/ram0/p0/bin/busybox", Root).error() == Error::Invalid);

                // Followed: stat, open (also for writing), directories, along a path.
                CHECK(Vfs::stat("/ram0/p0/bin/awk", Root).value().type == FileType::File);
                CHECK(Vfs::stat("/ram0/p0/bin/awk", Root).value().id == Vfs::stat("/ram0/p0/bin/busybox", Root).value().id);
                CHECK(Vfs::stat("/ram0/p0/d", Root).value().isDirectory());
                CHECK(get("/ram0/p0/d/notes.txt") == "notes");
                CHECK(Vfs::stat("/ram0/p0/d/notes.txt", Root).value().size == 5);
                put("/ram0/p0/d/new.txt", "made through the link");
                CHECK(get("/ram0/p0/data/new.txt") == "made through the link");
                CHECK(Vfs::mkdir("/ram0/p0/d/sub", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/data/sub", Root).value().isDirectory());
                {
                        auto [dir, err] = Vfs::openDirectory("/ram0/p0/d", Root);
                        REQUIRE(err.isOk());
                        size_t n = 0;
                        while (dir.next().error().isOk()) ++n;
                        CHECK(n == 3);
                }
                CHECK(Vfs::realPath("/ram0/p0/d/sub/..", Root).value() == "/ram0/p0/data");
                CHECK(Vfs::realPath("/ram0/p0/bin/awk", Root).value() == "/ram0/p0/bin/busybox");
                CHECK(Vfs::stat("x", Vfs::Context{"/ram0/p0/d", nullptr}).error() == Error::NotExist);
                CHECK(Vfs::stat("notes.txt", Vfs::Context{"/ram0/p0/d", nullptr}).isOk());
                // Truncating through a link cuts the target, not the link.
                put("/ram0/p0/bin/awk", "short");
                CHECK(get("/ram0/p0/bin/busybox") == "short");
                CHECK(Vfs::lstat("/ram0/p0/bin/awk", Root).value().type == FileType::Link);

                // Listing: links as links.
                {
                        auto [dir, err] = Vfs::openDirectory("/ram0/p0/bin", Root);
                        REQUIRE(err.isOk());
                        int links = 0;
                        for (auto r = dir.next(); r.isOk(); r = dir.next()) links += r.value().type == FileType::Link;
                        CHECK(links == 1);
                }

                // A dangling link, a loop.
                CHECK(Vfs::symlink("nowhere", "/ram0/p0/dangling", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/dangling", Root).error() == Error::NotExist);
                CHECK(Vfs::lstat("/ram0/p0/dangling", Root).value().type == FileType::Link);
                CHECK(Vfs::symlink("loop2", "/ram0/p0/loop1", Root).isOk());
                CHECK(Vfs::symlink("loop1", "/ram0/p0/loop2", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/loop1", Root).error() == Error::TooManyLinks);
                CHECK(Vfs::open("/ram0/p0/loop1", Vfs::Read, Root).error() == Error::TooManyLinks);
                CHECK(Vfs::stat("/ram0/p0/loop1/x", Root).error() == Error::TooManyLinks);

                // The link itself: removed, renamed; rmdir refuses it.
                CHECK(Vfs::rmdir("/ram0/p0/d", Root) == Error::NotDir);
                CHECK(Vfs::rename("/ram0/p0/d", "/ram0/p0/e", Vfs::RenameMode::NoReplace, Root).isOk());
                CHECK(Vfs::readLink("/ram0/p0/e", Root).value() == "/ram0/p0/data");
                CHECK(Vfs::remove("/ram0/p0/e", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/data/notes.txt", Root).isOk());
                CHECK(Vfs::stat("/ram0/p0/e", Root).error() == Error::NotExist);

                // resolve(): realPath(), stat() and the built-in program in one lookup.
                {
                        put("/ram0/p0/bin/sed", "SED");
                        auto [found, err] = Vfs::resolve("awk", Vfs::Context{"/ram0/p0/bin", nullptr});
                        REQUIRE(err.isOk());
                        CHECK(found.path == "/ram0/p0/bin/busybox");
                        CHECK(found.program == nullptr);
                        const FileInfo st = Vfs::stat("/ram0/p0/bin/awk", Root).value();
                        CHECK(found.info.id == st.id);
                        CHECK(found.info.device == st.device);
                        CHECK(found.info.size == st.size);
                        CHECK(Vfs::resolve("/ram0/p0/bin/nope", Root).error() == Error::NotExist);
                        CHECK(Vfs::resolve("/ram0/p0/loop1", Root).error() == Error::TooManyLinks);
                }
                // A link's target is kept between lookups (devplan/programs.md §19), and not past
                // a change: a link replaced at once by one of the same size is read again.
                put("/ram0/p0/bin/toolbox", "TB");
                CHECK(Vfs::realPath("/ram0/p0/bin/awk", Root).value() == "/ram0/p0/bin/busybox");
                CHECK(Vfs::remove("/ram0/p0/bin/awk", Root).isOk());
                CHECK(Vfs::symlink("toolbox", "/ram0/p0/bin/awk", Root).isOk());
                CHECK(Vfs::realPath("/ram0/p0/bin/awk", Root).value() == "/ram0/p0/bin/toolbox");
                CHECK(get("/ram0/p0/bin/awk") == "TB");
                CHECK(Vfs::symlink("busybox", "/ram0/p0/bin/gawk", Root).isOk());
                CHECK(Vfs::rename("/ram0/p0/bin/gawk", "/ram0/p0/bin/awk", Vfs::RenameMode::Replace, Root).isOk());
                CHECK(Vfs::realPath("/ram0/p0/bin/awk", Root).value() == "/ram0/p0/bin/busybox");
                CHECK(Vfs::remove("/ram0/p0/bin/toolbox", Root).isOk());
                CHECK(Vfs::remove("/ram0/p0/bin/sed", Root).isOk());

                // The cookie alone doesn't make a link: only a System file is one.
                put("/ram0/p0/plain", "!<symlink>\xff\xfex\0\0\0");
                CHECK(Vfs::lstat("/ram0/p0/plain", Root).value().type == FileType::File);
                CHECK(liveObjects() == base);
        }
}

TEST_CASE("Vfs: /dev/null, /dev/zero, /dev/urandom and /dev/console") {
        fresh();
        CHECK(Vfs::stat("/", Root).value().isDirectory());
        CHECK(Vfs::stat("/dev/null", Root).value().type == FileType::Device);
        CHECK(Vfs::stat("/dev/nope", Root).error() == Error::NotExist);
        CHECK(Vfs::stat("/dev/null/x", Root).error() == Error::NotExist);
        CHECK(Vfs::openDirectory("/dev/null", Root).error() == Error::NotDir);
        CHECK(Vfs::open("/", Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::statfs("/dev", Root).error() == Error::NotSupported);
        {
                FileHandle::Ptr null = open("/dev/null", Vfs::Read | Vfs::Write);
                CHECK(null->write(bmds::StringView("gone"), WaitForever).error().isOk());
                uint8_t buf[4];
                CHECK(null->read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);
        }
        {
                FileHandle::Ptr zero = open("/dev/zero", Vfs::Read | Vfs::Write);
                CHECK(zero->write(bmds::StringView("gone"), WaitForever).error().isOk());
                uint8_t buf[300];
                std::memset(buf, 0x5a, sizeof(buf));
                CHECK(zero->read(WritableByteSpan(buf), WaitForever).value() == sizeof(buf));
                CHECK(std::count(buf, buf + sizeof(buf), 0) == static_cast<long>(sizeof(buf)));
                CHECK(Vfs::stat("/dev/zero", Root).value().id == zero->id());
        }
        for (const char *path : {"/dev/urandom", "/dev/random"}) {
                FileHandle::Ptr random = open(path, Vfs::Read | Vfs::Write);
                CHECK(random->write(bmds::StringView("seed"), WaitForever).error().isOk());
                uint8_t a[64] = {};
                uint8_t b[64] = {};
                CHECK(random->read(WritableByteSpan(a), WaitForever).value() == sizeof(a));
                CHECK(random->read(WritableByteSpan(b), WaitForever).value() == sizeof(b));
                CHECK(std::memcmp(a, b, sizeof(a)) != 0); // 2^-512 to fail
                CHECK(Vfs::stat(path, Root).value().type == FileType::Device);
        }
        CHECK(Vfs::stat("/dev/random", Root).value().id != Vfs::stat("/dev/urandom", Root).value().id);
        CHECK(Vfs::remove("/dev/zero", Root) == Error::PermissionDenied);
        CHECK(Vfs::open("/dev/console", Vfs::Write, Root).error() == Error::NotExist);
        bmds::MemoryStream terminal(bmds::MemSpace::Psram);
        {
                const Vfs::Context withTerminal{"/", &terminal};
                FileHandle::Ptr    console = open("/dev/console", Vfs::Write, withTerminal);
                CHECK(console->write(bmds::StringView("hi"), WaitForever).error().isOk());
                // /dev/tty is the same: the caller's terminal.
                FileHandle::Ptr tty = open("/dev/tty", Vfs::Write, withTerminal);
                CHECK(tty->write(bmds::StringView("!"), WaitForever).error().isOk());
        }
        CHECK(terminal.contents() == "hi!");
        CHECK(Vfs::open("/dev/tty", Vfs::Write, Root).error() == Error::NotExist);
}

TEST_CASE("Vfs: time stamps") {
        fresh();
        REQUIRE(bmds::Clock::setTimeZone("CET-1CEST,M3.5.0,M10.5.0/3").isOk());
        const int64_t when = bmds::Calendar::daysFromCivil(2026, 6, 15) * 86400 + 12 * 3600 + 34 * 60 + 56;
        REQUIRE(bmds::Clock::set(Timestamp::fromSeconds(when), bmds::ClockSource::Manual).isOk());
        put("/ram0/p0/stamped.txt", "x");
        const int64_t stamped = Vfs::stat("/ram0/p0/stamped.txt", Root).value().modified.seconds();
        CHECK(stamped >= when - 2); // FAT keeps even seconds
        CHECK(stamped <= when + 2);

        const int64_t old = bmds::Calendar::daysFromCivil(1999, 12, 31) * 86400 + 23 * 3600 + 59 * 60 + 58;
        CHECK(Vfs::setModified("/ram0/p0/stamped.txt", Timestamp::fromSeconds(old), Root).isOk());
        CHECK(Vfs::stat("/ram0/p0/stamped.txt", Root).value().modified.seconds() == old);
        REQUIRE(bmds::Clock::setTimeZone("UTC0").isOk());
        CHECK(Vfs::stat("/ram0/p0/stamped.txt", Root).value().modified.seconds() == old + 3600); // stored as CET
}

TEST_CASE("Vfs: a full disk") {
        fresh(FsFormat::Fat32);
        const uint64_t       free = Vfs::statfs("/ram0/p0", Root).value().freeBytes;
        FileHandle::Ptr      h = open("/ram0/p0/fill", Vfs::Write | Vfs::Create);
        std::vector<uint8_t> chunk(1 << 20, 0x5a);
        uint64_t             written = 0;
        Error                err;
        while (err.isOk()) {
                auto [n, e] = h->write(ByteSpan(chunk.data(), chunk.size()), WaitForever);
                err = e;
                if (e.isOk()) written += n;
        }
        CHECK(err == Error::NoSpace);
        CHECK(written <= free);
        CHECK(written + chunk.size() > free);
        CHECK(Vfs::statfs("/ram0/p0", Root).value().freeBytes < chunk.size());
}

namespace {

        // A RAM disk that counts the reads reaching it.
        class CountingDevice final : public bmds::BlockDevice {
                public:
                        CountingDevice() : _disk(512) {
                                REQUIRE(_disk.allocate(64 * 2048, bmds::MemSpace::Psram).isOk());
                        }

                        uint32_t sectorSize() const override { return _disk.sectorSize(); }
                        uint64_t sectorCount() const override { return _disk.sectorCount(); }
                        Error    sync() override { return _disk.sync(); }

                        size_t reads = 0;   // requests
                        size_t sectors = 0; // sectors they read

                protected:
                        Error readSectors(uint64_t sector, WritableByteSpan buf) override {
                                ++reads;
                                sectors += buf.size() / 512;
                                return _disk.read(sector, buf);
                        }
                        Error writeSectors(uint64_t sector, ByteSpan data) override {
                                return _disk.write(sector, data);
                        }

                private:
                        RamBlockDevice _disk;
        };

} // namespace

TEST_CASE("Vfs: the metadata cache") {
        disk();
        for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                CAPTURE(static_cast<int>(format));
                CountingDevice dev;
                char           name[MaxName + 1];
                REQUIRE(bmds::BlockDevices::addNumbered(dev, "cnt", name).isOk());
                REQUIRE(Vfs::format(dev, format).isOk());
                REQUIRE(Vfs::automount(dev).value() == 1);
                const std::string root = std::string("/") + name + "/p0";
                REQUIRE(Vfs::mkdir((root + "/bin").c_str(), Root).isOk());
                std::vector<std::string> paths;
                for (int i = 0; i < 120; ++i) paths.push_back(root + "/bin/applet-number-" + std::to_string(i));
                for (const std::string &p : paths) put(p.c_str(), "x");

                // Looked up again, a directory comes from memory.
                for (const std::string &p : paths) REQUIRE(Vfs::stat(p.c_str(), Root).isOk());
                dev.reads = 0;
                for (const std::string &p : paths) REQUIRE(Vfs::stat(p.c_str(), Root).isOk());
                CHECK(dev.reads == 0);

                // A directory bigger than the cache (2,000 names of three entries each, about 190 KB) on
                // another disk, read through once, leaves the one in use cached.
                {
                        CountingDevice other;
                        char           otherName[MaxName + 1];
                        REQUIRE(bmds::BlockDevices::addNumbered(other, "cnt", otherName).isOk());
                        REQUIRE(Vfs::format(other, format).isOk());
                        REQUIRE(Vfs::automount(other).value() == 1);
                        const std::string huge = std::string("/") + otherName + "/p0/huge";
                        REQUIRE(Vfs::mkdir(huge.c_str(), Root).isOk());
                        for (int i = 0; i < 2000; ++i)
                                put((huge + "/a-long-file-name-" + std::to_string(i)).c_str(), "");
                        Vfs::eject(other); // its sectors out of the cache
                        REQUIRE(Vfs::automount(other).value() == 1);
                        for (const std::string &p : paths) REQUIRE(Vfs::stat(p.c_str(), Root).isOk());
                        CHECK(list(huge.c_str()).size() == 2000);
                        dev.reads = 0;
                        bool found = true; // every name, whatever its hash shares
                        for (int i = 0; i < 2000; ++i)
                                found = found &&
                                        Vfs::stat((huge + "/a-long-file-name-" + std::to_string(i)).c_str(), Root)
                                                .isOk();
                        CHECK(found);
                        CHECK(Vfs::stat((huge + "/a-long-file-name-2000").c_str(), Root).error() == Error::NotExist);
                        CHECK(Vfs::stat((huge + "/A-LONG-FILE-NAME-1999").c_str(), Root).isOk()); // case folded
                        for (int i = 0; i < 2000; ++i)
                                (void)Vfs::stat((huge + "/a-long-file-name-" + std::to_string(i)).c_str(), Root);
                        for (const std::string &p : paths) REQUIRE(Vfs::stat(p.c_str(), Root).isOk());
                        CHECK(list(huge.c_str()).size() == 2000);
                        dev.reads = 0;
                        for (const std::string &p : paths) REQUIRE(Vfs::stat(p.c_str(), Root).isOk());
                        CHECK(dev.reads == 0);
                        Vfs::eject(other);
                        bmds::BlockDevices::remove(other);
                }

                // Evictions, and writes kept in step.
                REQUIRE(Vfs::mkdir((root + "/big").c_str(), Root).isOk());
                for (int i = 0; i < 600; ++i)
                        put((root + "/big/a-long-file-name-" + std::to_string(i)).c_str(), std::to_string(i));
                CHECK(list((root + "/big").c_str()).size() == 600);
                for (int i = 0; i < 600; i += 2)
                        REQUIRE(Vfs::remove((root + "/big/a-long-file-name-" + std::to_string(i)).c_str(), Root)
                                        .isOk());
                CHECK(list((root + "/big").c_str()).size() == 300);
                CHECK(get((root + "/big/a-long-file-name-599").c_str()) == "599");

                // Remounted: read from the device again, and still right; read ahead in runs.
                Vfs::eject(dev);
                REQUIRE(Vfs::automount(dev).value() == 1);
                dev.reads = 0;
                dev.sectors = 0;
                CHECK(list((root + "/big").c_str()).size() == 300);
                CHECK(dev.reads > 0);
                CHECK(dev.sectors >= 4 * dev.reads);
                Vfs::eject(dev);
                bmds::BlockDevices::remove(dev);
        }
}

namespace {

        // A card for the write-back tests: a base image (read-only once made) and the sectors written
        // over it; every write can be recorded, in order, and replayed onto a fresh overlay.
        class OverlayDevice final : public bmds::BlockDevice {
                public:
                        static constexpr uint64_t Sectors = 131072; // 64 MB: enough clusters for FAT32

                        uint32_t sectorSize() const override { return 512; }
                        uint64_t sectorCount() const override { return Sectors; }
                        Error    sync() override { return Error::Ok; }

                        using Image = std::map<uint64_t, std::array<uint8_t, 512>>;

                        Image                                                      base;    // the card before
                        Image                                                      overlay; // written since
                        bool                                                       recording = false;
                        std::vector<std::pair<uint64_t, std::array<uint8_t, 512>>> writes;

                protected:
                        Error readSectors(uint64_t sector, WritableByteSpan buf) override {
                                for (size_t k = 0; k < buf.size() / 512; ++k) {
                                        auto                            at = overlay.find(sector + k);
                                        const std::array<uint8_t, 512> *from = nullptr;
                                        if (at != overlay.end()) {
                                                from = &at->second;
                                        } else if (auto b = base.find(sector + k); b != base.end()) {
                                                from = &b->second;
                                        }
                                        if (from != nullptr) {
                                                std::memcpy(buf.data() + k * 512, from->data(), 512);
                                        } else {
                                                std::memset(buf.data() + k * 512, 0, 512);
                                        }
                                }
                                return Error::Ok;
                        }

                        Error writeSectors(uint64_t sector, ByteSpan data) override {
                                for (size_t k = 0; k < data.size() / 512; ++k) {
                                        std::array<uint8_t, 512> bytes;
                                        std::memcpy(bytes.data(), data.data() + k * 512, 512);
                                        overlay[sector + k] = bytes;
                                        if (recording) writes.emplace_back(sector + k, bytes);
                                }
                                return Error::Ok;
                        }
        };

        std::string pattern(int file, size_t size) {
                std::string out(size, '\0');
                for (size_t i = 0; i < size; ++i) out[i] = char('a' + (file * 7 + int(i) * 13) % 26);
                return out;
        }

} // namespace

TEST_CASE("Vfs: metadata written back survives a cut at any write") {
        disk();
        // Where the burst is synced as well (the flusher's timer can split it anywhere): none, after
        // the removals, after the folder, after the new files' first half. Twenty files of each, so
        // entry blocks cross sectors and a cut leaves some broken.
        for (int split = 0; split < 4; ++split) {
                for (FsFormat format : {FsFormat::Fat32, FsFormat::ExFat}) {
                        CAPTURE(split);
                        CAPTURE(static_cast<int>(format));
                        OverlayDevice dev;
                        char          name[MaxName + 1];
                        REQUIRE(bmds::BlockDevices::addNumbered(dev, "cut", name).isOk());
                        const std::string root = std::string("/") + name + "/p0";
                        const auto        path = [&](const std::string &rest) {
                                return root + rest;
                        };
                        const auto sizeOf = [](int i) {
                                return size_t(700 + i * 450);
                        }; // several clusters

                        // The card before: files a0..a19 of several clusters, written and synced.
                        REQUIRE(Vfs::format(dev, format).isOk());
                        REQUIRE(Vfs::automount(dev).value() == 1);
                        for (int i = 0; i < 20; ++i) put(path("/a" + std::to_string(i)).c_str(), pattern(i, sizeOf(i)));
                        REQUIRE(Vfs::sync().isOk());
                        Vfs::eject(dev);
                        for (auto &[sector, bytes] : dev.overlay) dev.base[sector] = bytes;
                        dev.overlay.clear();
                        bmds::Thread::sleep(1100); // a flush the base scheduled: done before the burst

                        // One burst: half of them removed, a folder of many names made and emptied (its
                        // clusters free again, perhaps used for data), new files b0..b19; each write recorded.
                        REQUIRE(Vfs::automount(dev).value() == 1);
                        dev.recording = true;
                        for (int i = 0; i < 20; i += 2)
                                REQUIRE(Vfs::remove(path("/a" + std::to_string(i)).c_str(), Root).isOk());
                        if (split == 1) REQUIRE(Vfs::sync().isOk());
                        REQUIRE(Vfs::mkdir(path("/d").c_str(), Root).isOk());
                        for (int i = 0; i < 40; ++i)
                                put(path("/d/a-long-name-to-fill-the-folder-" + std::to_string(i)).c_str(), "");
                        for (int i = 0; i < 40; ++i)
                                REQUIRE(Vfs::remove(
                                                path("/d/a-long-name-to-fill-the-folder-" + std::to_string(i)).c_str(),
                                                Root)
                                                .isOk());
                        REQUIRE(Vfs::rmdir(path("/d").c_str(), Root).isOk());
                        if (split == 2) REQUIRE(Vfs::sync().isOk());
                        for (int i = 0; i < 20; ++i) {
                                put(path("/b" + std::to_string(i)).c_str(), pattern(100 + i, sizeOf(i)));
                                if (split == 3 && i == 9) REQUIRE(Vfs::sync().isOk());
                        }
                        REQUIRE(Vfs::sync().isOk());
                        dev.recording = false;
                        Vfs::eject(dev);
                        const auto writes = dev.writes;
                        CHECK(writes.size() > 20);

                        // Cut after each write: whatever is listed reads back right, and a file written
                        // afterwards overwrites none of it (no entry points at clusters marked free).
                        bool consistent = true;
                        for (size_t cut = 0; cut <= writes.size() && consistent; ++cut) {
                                CAPTURE(cut);
                                dev.overlay.clear();
                                for (size_t w = 0; w < cut; ++w) dev.overlay[writes[w].first] = writes[w].second;
                                REQUIRE(Vfs::automount(dev).value() == 1);
                                const auto check = [&]() {
                                        for (const std::string &n : list(root.c_str())) {
                                                if (n.empty() || n.back() == '/' || n == "c") continue;
                                                const bool        isA = n[0] == 'a';
                                                const int         i = std::stoi(n.substr(1));
                                                const std::string want = pattern(isA ? i : 100 + i, sizeOf(i));
                                                auto [h, oe] = Vfs::open(path("/" + n).c_str(), Vfs::Read, Root);
                                                if (oe.isError()) {
                                                        MESSAGE("cut ", cut, ": ", n,
                                                                " listed but can't be opened: ", oe.name());
                                                        return false;
                                                }
                                                h.reset();
                                                // A file whose size wasn't written yet is shorter, never wrong.
                                                const std::string got = get(path("/" + n).c_str());
                                                if (got.size() > want.size() || want.compare(0, got.size(), got) != 0 ||
                                                    (isA && got.size() != want.size())) {
                                                        MESSAGE("cut ", cut, ": ", n, " read ", got.size(),
                                                                " bytes of ", want.size());
                                                        return false;
                                                }
                                        }
                                        return true;
                                };
                                consistent = check();
                                auto [c, ce] =
                                        Vfs::open(path("/c").c_str(), Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                                if (ce.isError()) MESSAGE("cut ", cut, ": a new file can't be made: ", ce.name());
                                consistent = consistent && ce.isOk();
                                const std::string big = pattern(999, 16384);
                                if (ce.isOk())
                                        consistent = consistent &&
                                                     c->write(ByteSpan(reinterpret_cast<const uint8_t *>(big.data()),
                                                                       big.size()),
                                                              WaitForever)
                                                             .error()
                                                             .isOk();
                                c.reset();
                                if (auto [h2, e2] = Vfs::open(path("/c").c_str(), Vfs::Read, Root); e2.isError()) {
                                        std::string names;
                                        for (const std::string &n : list(root.c_str())) names += n + " ";
                                        MESSAGE("cut ", cut, ": c written, then opening it: ", std::string(e2.name()),
                                                "; listed: ", names);
                                        consistent = false;
                                        Vfs::eject(dev);
                                        break;
                                }
                                consistent = consistent && check() && get(path("/c").c_str()) == big;
                                Vfs::eject(dev);
                        }
                        CHECK(consistent);
                        bmds::BlockDevices::remove(dev);
                }
        }
}

namespace {

        constexpr bmds::Command FsCommandTable[] = {
                {"mount", "[DEVICE PATH]", "mount", bmds::FsCommands::mount},
                {"umount", "PATH", "umount", bmds::FsCommands::umount},
        };

} // namespace

TEST_CASE("mount and umount") {
        fresh();
        FakeConsole                  con;
        std::unique_ptr<bmds::Shell> shell =
                std::make_unique<bmds::Shell>(con, bmds::Span<const bmds::Command>(FsCommandTable), 1 << 20);
        REQUIRE(shell->init().isOk());
        auto out = [&](const char *line, int status = bmds::Shell::ExitSuccess) {
                CHECK(shell->execute(line) == status);
                return con.take();
        };
        CHECK(out("mount") == "/dev/ram0p0 on /ram0/p0 type fat32\n");
        CHECK(out("umount /ram0/p0") == "");
        CHECK(out("mount") == "");
        CHECK(out("umount /ram0/p0", bmds::Shell::ExitFailure) == "umount: /ram0/p0: not mounted\n");
        CHECK(out("mount /dev/ram0p0 /data") == "");
        CHECK(out("mount") == "/dev/ram0p0 on /data type fat32\n");
        CHECK(out("mount /dev/ram0p0 /again", bmds::Shell::ExitFailure) == "mount: /again: Device or resource busy\n");
        CHECK(out("mount /dev/nope /x", bmds::Shell::ExitFailure) == "mount: /dev/nope: not a block device\n");
        CHECK(out("mount /dev/ram0p0", bmds::Shell::ExitUsage) == "usage: mount [DEVICE PATH]\n");
        CHECK(out("umount /data") == "");
}

namespace {

        bool comesAndGoesThere = false;

} // namespace

TEST_CASE("Vfs: added /dev files that come and go, and devices among them") {
        fresh();
        Vfs::DeviceFile file;
        file.name = "comesandgoes";
        file.open = [](void *, uint8_t) -> bmds::Result<FileHandle::Ptr> {
                return bmds::makeError<FileHandle::Ptr>(Error::Busy);
        };
        file.present = [](void *) {
                return comesAndGoesThere;
        };
        file.isDevice = true;
        REQUIRE(Vfs::addDeviceFile(file) == Error::Ok);
        CHECK(Vfs::addDeviceFile(file) == Error::Exists); // gone, but its name is still taken
        auto listed = [] {
                const std::vector<std::string> names = list("/dev");
                return std::find(names.begin(), names.end(), "comesandgoes") != names.end();
        };
        // Gone: not there to stat, open or list.
        comesAndGoesThere = false;
        CHECK(Vfs::stat("/dev/comesandgoes", Root).error() == Error::NotExist);
        CHECK(Vfs::open("/dev/comesandgoes", Vfs::Read, Root).error() == Error::NotExist);
        CHECK_FALSE(listed());
        // There: a device (nothing reads it to see what it is), opened by its call.
        comesAndGoesThere = true;
        auto [info, err] = Vfs::stat("/dev/comesandgoes", Root);
        REQUIRE(err == Error::Ok);
        CHECK(info.type == FileType::Device);
        CHECK(Vfs::open("/dev/comesandgoes", Vfs::Read, Root).error() == Error::Busy);
        CHECK(listed());
        comesAndGoesThere = false;
}

TEST_CASE("Vfs: /etc, built from the settings when opened") {
        disk();
        static int                built = 0;
        static const Vfs::EtcFile Hosts = {"hosts",
                                           [](void *, bmds::String &out) {
                                                   ++built;
                                                   return out.appendf("127.0.0.1\tlocalhost # %d\n", built);
                                           },
                                           nullptr};
        static bool               added = false;
        if (!added) {
                REQUIRE(Vfs::addEtcFile(Hosts).isOk());
                added = true;
        }
        CHECK(Vfs::addEtcFile(Hosts) == Error::Exists);
        CHECK(Vfs::addEtcFile(Vfs::EtcFile{"a/b", Hosts.generate, nullptr}) == Error::Invalid);
        CHECK(Vfs::addEtcFile(Vfs::EtcFile{"none", nullptr, nullptr}) == Error::Invalid);

        // Built at each open, nothing for a look: stat says 0 (as /proc does).
        const int before = built;
        CHECK(Vfs::stat("/etc", Root).value().isDirectory());
        CHECK(Vfs::stat("/etc/hosts", Root).value().type == FileType::File);
        CHECK(Vfs::stat("/etc/hosts", Root).value().size == 0);
        CHECK(Vfs::stat("/etc/nope", Root).error() == Error::NotExist);
        CHECK(list("/etc") == std::vector<std::string>{"hosts"});
        CHECK(built == before);
        CHECK(get("/etc/hosts") == "127.0.0.1\tlocalhost # " + std::to_string(before + 1) + "\n");
        CHECK(get("/etc/../etc/./hosts") == "127.0.0.1\tlocalhost # " + std::to_string(before + 2) + "\n");

        // Seekable; read-only; nothing mounts there.
        FileHandle::Ptr h = open("/etc/hosts", Vfs::Read);
        CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::End).value() == 24); // "127.0.0.1\tlocalhost # N\n"
        CHECK(h->stream().seek(10, bmds::Stream::SeekFrom::Start).value() == 10);
        uint8_t buf[9];
        CHECK(h->read(WritableByteSpan(buf), WaitForever).value() == 9);
        CHECK(std::string(reinterpret_cast<char *>(buf), 9) == "localhost");
        h.reset();
        CHECK(Vfs::open("/etc/hosts", Vfs::Write, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/etc/new", Vfs::Write | Vfs::Create, Root).error() == Error::ReadOnly);
        CHECK(Vfs::open("/etc", Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::remove("/etc/hosts", Root) == Error::ReadOnly);
        CHECK(Vfs::mkdir("/etc/sub", Root) == Error::ReadOnly);
        CHECK(Vfs::mount("/etc", disk().device) == Error::Invalid);
        CHECK(Vfs::openDirectory("/etc/hosts", Root).error() == Error::NotDir);
}

namespace {

        // A directory of numbered devices (as /dev/pts): entries 1 and 3 exist.
        bool numberedPresent(void *, size_t index) {
                return index == 1 || index == 3;
        }

        size_t numberedOpened = 99;

        bmds::Result<FileHandle::Ptr> numberedOpen(void *, size_t index, uint8_t) {
                numberedOpened = index;
                return bmds::makeError<FileHandle::Ptr>(Error::Busy);
        }

} // namespace

TEST_CASE("Vfs: an added /dev directory of numbered devices") {
        fresh();
        static const Vfs::DeviceDirectory Numbered{"numbered", 4, numberedPresent, numberedOpen, nullptr};
        static bool                       added = false;
        if (!added) {
                REQUIRE(Vfs::addDeviceDirectory(Numbered) == Error::Ok);
                added = true;
        }
        CHECK(Vfs::addDeviceDirectory(Numbered) == Error::Exists);
        CHECK(Vfs::addDeviceDirectory(Vfs::DeviceDirectory{"null", 1, nullptr, numberedOpen, nullptr}) ==
              Error::Exists);
        CHECK(Vfs::addDeviceDirectory(Vfs::DeviceDirectory{"a/b", 1, nullptr, numberedOpen, nullptr}) ==
              Error::Invalid);
        const std::vector<std::string> dev = list("/dev");
        CHECK(std::find(dev.begin(), dev.end(), "numbered/") != dev.end());
        CHECK(Vfs::stat("/dev/numbered", Root).value().isDirectory());
        CHECK(list("/dev/numbered") == std::vector<std::string>{"1", "3"});
        CHECK(Vfs::stat("/dev/numbered/1", Root).value().type == FileType::Device);
        CHECK(Vfs::stat("/dev/numbered/2", Root).error() == Error::NotExist);  // not present
        CHECK(Vfs::stat("/dev/numbered/4", Root).error() == Error::NotExist);  // beyond its count
        CHECK(Vfs::stat("/dev/numbered/01", Root).error() == Error::NotExist); // not its name
        CHECK(Vfs::stat("/dev/numbered/3/x", Root).error() == Error::NotExist);
        CHECK(Vfs::open("/dev/numbered/3", Vfs::Read, Root).error() == Error::Busy);
        CHECK(numberedOpened == 3);
        CHECK(Vfs::open("/dev/numbered/../numbered/1", Vfs::Read, Root).error() == Error::Busy);
        CHECK(numberedOpened == 1);
        CHECK(Vfs::open("/dev/numbered/2", Vfs::Read, Root).error() == Error::NotExist);
        CHECK(Vfs::open("/dev/numbered", Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::remove("/dev/numbered/1", Root) == Error::PermissionDenied);
}

TEST_CASE("VFS: renames onto a taken name on FAT (POSIX's replace, and NoReplace)") {
        fresh();
        RenameChecks::run("/ram0/p0/r", false);
}
