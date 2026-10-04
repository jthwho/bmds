/**
 * @file      vfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/clock.h>
#include <bmds/fscommands.h>
#include <bmds/memorystream.h>
#include <bmds/partitions.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"
#include "hostmemory.h"

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

        CHECK(list("/") == std::vector<std::string>{"dev/", "ram0/"});
        CHECK(list("/ram0") == std::vector<std::string>{"p0/"});
        CHECK(list("/dev") == std::vector<std::string>{"console", "null", "ram0", "ram0p0"});
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
                CHECK(list("/") == std::vector<std::string>{"dev/"});
                CHECK(Vfs::stat("/ram0", Root).error() == Error::NotExist);
                CHECK(Vfs::mount("/data/main", *p0).isOk());
                CHECK(Vfs::stat("/data/../data/main/../main/.", Root).value().isDirectory());
                CHECK(list("/") == std::vector<std::string>{"data/", "dev/"});
                CHECK(Vfs::mount("/data/main", *p0) == Error::Exists);
                CHECK(Vfs::mount("/other", *p0) == Error::Busy);
                CHECK(Vfs::mount("/other", ram) == Error::Busy); // the whole device under a mounted partition
                CHECK(Vfs::mount("/data/main/inner", ram) == Error::Invalid);
                CHECK(Vfs::mount("/data", ram) == Error::Invalid);
                CHECK(Vfs::mount("/dev/x", ram) == Error::Invalid);
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
                CHECK(list("/dev") == std::vector<std::string>{"console", "null", "ram0"});
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
                CHECK(Vfs::rename("/ram0/p0/docs/a.txt", "/ram0/p0/docs/b.txt", Root).isOk());
                CHECK(Vfs::rename("b.txt", "deeper/c.txt", inDocs).isOk());
                CHECK(get("/ram0/p0/docs/deeper/c.txt") == "A");
                CHECK(Vfs::stat("/ram0/p0/docs/a.txt", Root).error() == Error::NotExist);
                put("/ram0/p0/docs/d.txt", "D");
                CHECK(Vfs::rename("/ram0/p0/docs/d.txt", "/ram0/p0/docs/deeper/c.txt", Root) == Error::Exists);
                CHECK(Vfs::rename("/ram0/p0/docs", "/ram0/p0/papers", Root).isOk());
                CHECK(get("/ram0/p0/papers/deeper/c.txt") == "A");
                CHECK(Vfs::rename("/ram0/p0/papers/d.txt", "/dev/d.txt", Root) == Error::NotSupported);
                CHECK(Vfs::rename("/ram0/p0/papers/d.txt", "/elsewhere/d.txt", Root) == Error::NotSupported);
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

TEST_CASE("Vfs: /dev/null and /dev/console") {
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
        CHECK(Vfs::open("/dev/console", Vfs::Write, Root).error() == Error::NotExist);
        bmds::MemoryStream terminal(bmds::MemSpace::Psram);
        {
                const Vfs::Context withTerminal{"/", &terminal};
                FileHandle::Ptr    console = open("/dev/console", Vfs::Write, withTerminal);
                CHECK(console->write(bmds::StringView("hi"), WaitForever).error().isOk());
        }
        CHECK(terminal.contents() == "hi");
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
