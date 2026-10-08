/**
 * @file      lfsvfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * littlefs in the VFS, on a simulated NOR flash (devplan/storage.md §13d):
 * what the FAT tests ask, the differences FAT hides (Busy, stale files), and
 * the ordered write queue cut off at random points.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/clock.h>
#include <bmds/directorywatch.h>
#include <bmds/flashqueue.h>
#include <bmds/ramflashdevice.h>
#include <bmds/vfs.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileInfo;
using bmds::FileType;
using bmds::FlashQueue;
using bmds::RamFlashDevice;
using bmds::Stream;
using bmds::Timestamp;
using bmds::WaitForever;
using bmds::WritableByteSpan;
namespace Vfs = bmds::Vfs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        constexpr uint32_t BlockBytes = 4096;

        // A path the VFS (StringView) and the helpers (std::string) both take.
        struct Path {
                        std::string text;
                        operator bmds::StringView() const { return bmds::StringView(text.data(), text.size()); }
                        operator const std::string &() const { return text; }
        };

        Path path(std::string text) {
                return Path{std::move(text)};
        }

        // A flash registered as a block device for one test, with littlefs on it at @p path;
        // gone (unmounted, unregistered) with the object.
        struct Flash {
                        explicit Flash(const char *mountPath, uint32_t blocks = 256, size_t queue = 0)
                            : path(mountPath) {
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(device.allocate(BlockBytes, blocks, bmds::MemSpace::Psram).isOk());
                                if (queue != 0) REQUIRE(device.setQueued(queue).isOk());
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(device, "rflash", name).isOk());
                                REQUIRE(Vfs::format(device, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::mount(path, device).isOk());
                        }

                        ~Flash() {
                                Vfs::eject(device);
                                bmds::BlockDevices::remove(device);
                        }

                        Path at(const std::string &name) const { return Path{std::string(path) + name}; }

                        RamFlashDevice device;
                        const char    *path;
        };

        FileHandle::Ptr open(const std::string &path, uint8_t flags) {
                auto [handle, err] = Vfs::open(Path{path}, flags, Root);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                return handle;
        }

        void put(const std::string &path, const std::string &text,
                 uint8_t flags = Vfs::Write | Vfs::Create | Vfs::Truncate) {
                FileHandle::Ptr h = open(path, flags);
                if (!text.empty())
                        REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                                         WaitForever)
                                        .error()
                                        .isOk());
        }

        std::string get(const std::string &path) {
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

        std::vector<std::string> list(const std::string &path) {
                auto [dir, err] = Vfs::openDirectory(Path{path}, Root);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                std::vector<std::string> names;
                for (;;) {
                        auto [info, nextErr] = dir.next();
                        if (nextErr == Error::EndOfFile) break;
                        REQUIRE(nextErr.isOk());
                        names.push_back(std::string(info.name.cStr()) + (info.isDirectory()            ? "/"
                                                                         : info.type == FileType::Link ? "@"
                                                                                                       : ""));
                }
                std::sort(names.begin(), names.end());
                return names;
        }

        std::string mountType(const char *path) {
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(bmds::Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i)
                        if (std::string(mounts[i].path) == path) return mounts[i].type;
                return "";
        }

} // namespace

TEST_CASE("littlefs in the VFS: files and directories") {
        Flash        f("/lfs");
        const size_t base = HostMemory::liveAllocations();
        CHECK(mountType("/lfs") == "littlefs");
        const bmds::FsInfo fs = Vfs::statfs("/lfs", Root).value();
        CHECK(std::string(fs.type) == "littlefs");
        CHECK(fs.totalBytes == 256u * BlockBytes);
        CHECK(fs.clusterBytes == BlockBytes);
        CHECK(fs.freeBytes < fs.totalBytes);
        CHECK(fs.freeBytes > fs.totalBytes - 4 * BlockBytes);

        put(f.at("/a.txt"), "hello");
        CHECK(get(f.at("/a.txt")) == "hello");
        put(f.at("/a.txt"), " world", Vfs::Write | Vfs::Append);
        CHECK(get(f.at("/a.txt")) == "hello world");
        CHECK(Vfs::stat(f.at("/a.txt"), Root).value().size == 11);
        CHECK(Vfs::open(f.at("/a.txt"), Vfs::Write | Vfs::Create | Vfs::Exclusive, Root).error() == Error::Exists);
        CHECK(Vfs::open(f.at("/none"), Vfs::Read, Root).error() == Error::NotExist);
        // A file bigger than a block, in pieces.
        std::string big;
        for (int i = 0; i < 3000; ++i) big += std::to_string(i) + ",";
        put(f.at("/big"), big);
        CHECK(get(f.at("/big")) == big);

        CHECK(Vfs::mkdir(f.at("/dir"), Root).isOk());
        CHECK(Vfs::mkdir(f.at("/dir"), Root) == Error::Exists);
        CHECK(Vfs::mkdir(f.at("/no/dir"), Root) == Error::NotExist);
        CHECK(Vfs::open(f.at("/dir"), Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::stat(f.at("/a.txt/x"), Root).error() == Error::NotExist);
        put(f.at("/dir/b"), "b");
        CHECK(Vfs::mkdir(f.at("/dir/sub"), Root).isOk());
        CHECK(list(f.at("")) == std::vector<std::string>{"a.txt", "big", "dir/"});
        CHECK(list(f.at("/dir")) == std::vector<std::string>{"b", "sub/"});
        CHECK(Vfs::openDirectory(f.at("/a.txt"), Root).error() == Error::NotDir);
        CHECK(Vfs::rmdir(f.at("/dir"), Root) == Error::NotEmpty);
        CHECK(Vfs::remove(f.at("/dir"), Root) == Error::IsDir);
        CHECK(Vfs::rmdir(f.at("/a.txt"), Root) == Error::NotDir);
        // Renames don't replace (as on FAT), and move between directories.
        CHECK(Vfs::rename(f.at("/a.txt"), f.at("/big"), Root) == Error::Exists);
        CHECK(Vfs::rename(f.at("/a.txt"), f.at("/dir/sub/a"), Root).isOk());
        CHECK(get(f.at("/dir/sub/a")) == "hello world");
        CHECK(Vfs::rename(f.at("/gone"), f.at("/x"), Root) == Error::NotExist);
        CHECK(Vfs::remove(f.at("/dir/sub/a"), Root).isOk());
        CHECK(Vfs::rmdir(f.at("/dir/sub"), Root).isOk());
        CHECK(Vfs::remove(f.at("/dir/b"), Root).isOk());
        CHECK(Vfs::rmdir(f.at("/dir"), Root).isOk());
        CHECK(list(f.at("")) == std::vector<std::string>{"big"});
        // Names keep their case, and are case-sensitive (unlike FAT's).
        put(f.at("/Case"), "C");
        CHECK(Vfs::stat(f.at("/case"), Root).error() == Error::NotExist);
        CHECK(Vfs::copyFile(f.at("/Case"), f.at("/copy"), Root).isOk());
        CHECK(get(f.at("/copy")) == "C");
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("littlefs in the VFS: holes, truncation, and Busy as on FAT") {
        Flash        f("/lfs");
        const size_t base = HostMemory::liveAllocations();
        {
                FileHandle::Ptr h = open(f.at("/h.bin"), Vfs::Read | Vfs::Write | Vfs::Create);
                Stream         &s = h->stream();
                CHECK(h->write(bmds::StringView("abc"), WaitForever).error().isOk());
                CHECK(s.seek(5000, Stream::SeekFrom::Start).value() == 5000);
                uint8_t c = 1;
                CHECK(h->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::EndOfFile);
                CHECK(s.seek(0, Stream::SeekFrom::End).value() == 3);
                CHECK(s.seek(5000, Stream::SeekFrom::Start).value() == 5000);
                CHECK(h->write(bmds::StringView("Z"), WaitForever).error().isOk());
                CHECK(s.flush(WaitForever).isOk());
                CHECK(s.truncate(2).isOk());
                CHECK(s.seek(0, Stream::SeekFrom::Current).value() == 5001);
                CHECK(s.seek(0, Stream::SeekFrom::End).value() == 2);
                CHECK(s.truncate(10).isOk());
                CHECK(s.seek(0, Stream::SeekFrom::End).value() == 10);
                CHECK(s.seek(-11, Stream::SeekFrom::End).error() == Error::Invalid);
                // Open for writing: no second open at all, no removing or renaming.
                CHECK(Vfs::open(f.at("/h.bin"), Vfs::Read, Root).error() == Error::Busy);
                CHECK(Vfs::open(f.at("/h.bin"), Vfs::Write, Root).error() == Error::Busy);
                CHECK(Vfs::remove(f.at("/h.bin"), Root) == Error::Busy);
                CHECK(Vfs::rename(f.at("/h.bin"), f.at("/i.bin"), Root) == Error::Busy);
        }
        CHECK(get(f.at("/h.bin")) == std::string("ab") + std::string(8, '\0'));
        put(f.at("/g.bin"), "x");
        {
                FileHandle::Ptr h = open(f.at("/g.bin"), Vfs::Write);
                CHECK(h->stream().seek(3000, Stream::SeekFrom::Start).isOk());
                CHECK(h->write(bmds::StringView("y"), WaitForever).error().isOk());
        }
        const std::string g = get(f.at("/g.bin"));
        CHECK(g.size() == 3001);
        CHECK(g.find_first_not_of('\0', 1) == 3000);
        {
                // Open for reading: more readers, no writer.
                FileHandle::Ptr r1 = open(f.at("/g.bin"), Vfs::Read);
                FileHandle::Ptr r2 = open(f.at("/g.bin"), Vfs::Read);
                CHECK(Vfs::open(f.at("/g.bin"), Vfs::Write, Root).error() == Error::Busy);
                CHECK(Vfs::remove(f.at("/g.bin"), Root) == Error::Busy);
                CHECK(r1->stream().truncate(0) == Error::BadDescriptor);
                CHECK(r1->write(bmds::StringView("w"), WaitForever).error() == Error::BadDescriptor);
        }
        {
                FileHandle::Ptr w = open(f.at("/g.bin"), Vfs::Write);
                uint8_t         c = 0;
                CHECK(w->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::BadDescriptor);
        }
        CHECK(Vfs::remove(f.at("/g.bin"), Root).isOk());
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("littlefs in the VFS: links, times and identity") {
        Flash f("/lfs");
        CHECK(Vfs::mkdir(f.at("/bin"), Root).isOk());
        CHECK(Vfs::mkdir(f.at("/data"), Root).isOk());
        put(f.at("/bin/busybox"), "BB");
        put(f.at("/data/notes.txt"), "notes");
        CHECK(Vfs::symlink("busybox", f.at("/bin/awk"), Root).isOk());
        CHECK(Vfs::symlink(f.at("/data"), f.at("/d"), Root).isOk());
        CHECK(Vfs::symlink("busybox", f.at("/bin/awk"), Root) == Error::Exists);
        CHECK(Vfs::symlink("x", f.at("/none/x"), Root) == Error::NotExist);
        CHECK(get(f.at("/bin/awk")) == "BB");
        const FileInfo awk = Vfs::lstat(f.at("/bin/awk"), Root).value();
        CHECK(awk.type == FileType::Link);
        CHECK(awk.size == 7);
        CHECK((awk.attributes & FileInfo::System) != 0);
        CHECK(Vfs::readLink(f.at("/bin/awk"), Root).value() == "busybox");
        CHECK(Vfs::readLink(f.at("/bin/busybox"), Root).error() == Error::Invalid);
        CHECK(Vfs::stat(f.at("/bin/awk"), Root).value().id == Vfs::stat(f.at("/bin/busybox"), Root).value().id);
        CHECK(get(f.at("/d/notes.txt")) == "notes");
        put(f.at("/d/new.txt"), "through the link");
        CHECK(get(f.at("/data/new.txt")) == "through the link");
        CHECK(Vfs::mkdir(f.at("/d/sub"), Root).isOk());
        CHECK(Vfs::stat(f.at("/data/sub"), Root).value().isDirectory());
        CHECK(list(f.at("/d")) == std::vector<std::string>{"new.txt", "notes.txt", "sub/"});
        CHECK(list(f.at("/bin")) == std::vector<std::string>{"awk@", "busybox"});
        CHECK(std::string(Vfs::realPath(f.at("/d/sub/.."), Root).value().cStr()) == f.at("/data").text);
        // Truncating through a link cuts the target, not the link.
        put(f.at("/bin/awk"), "short");
        CHECK(get(f.at("/bin/busybox")) == "short");
        CHECK(Vfs::lstat(f.at("/bin/awk"), Root).value().type == FileType::Link);

        // Times: a write's, and one set (to the nanosecond).
        const int64_t when = bmds::Calendar::daysFromCivil(2026, 6, 15) * 86400 + 12 * 3600;
        REQUIRE(bmds::Clock::set(Timestamp::fromSeconds(when), bmds::ClockSource::Manual).isOk());
        put(f.at("/stamped"), "x");
        const int64_t stamped = Vfs::stat(f.at("/stamped"), Root).value().modified.seconds();
        CHECK(stamped >= when);
        CHECK(stamped <= when + 2);
        const Timestamp old{Timestamp::fromSeconds(946684799).ns + 123456789};
        CHECK(Vfs::setModified(f.at("/stamped"), old, Root).isOk());
        CHECK(Vfs::stat(f.at("/stamped"), Root).value().modified.ns == old.ns);
        CHECK(Vfs::setModified(f.at("/data"), old, Root).isOk());
        CHECK(Vfs::stat(f.at("/data"), Root).value().modified.ns == old.ns);
        CHECK(Vfs::mkdir(f.at("/made"), Root).isOk());
        CHECK(Vfs::stat(f.at("/made"), Root).value().modified.seconds() >= when);
        // Identity: names, handles and listings agree.
        const FileInfo a = Vfs::stat(f.at("/data/notes.txt"), Root).value();
        CHECK(a.id != 0);
        CHECK(open(f.at("/data/notes.txt"), Vfs::Read)->id() == a.id);
        CHECK(Vfs::stat(f.at("/d/notes.txt"), Root).value().id == a.id);
}

TEST_CASE("littlefs in the VFS: directory watches") {
        Flash                f("/lfs");
        bmds::DirectoryWatch watch;
        CHECK(Vfs::mkdir(f.at("/dir"), Root).isOk());
        CHECK(watch.watch(f.at("/dir"), Root).isOk());
        put(f.at("/dir/file"), "x");
        CHECK(watch.takeChanges() == bmds::DirectoryWatch::Contents);
        CHECK(get(f.at("/dir/file")) == "x");
        CHECK(watch.takeChanges() == 0);
        CHECK(Vfs::rename(f.at("/dir/file"), f.at("/moved"), Root).isOk());
        CHECK(watch.takeChanges() == bmds::DirectoryWatch::Entries);
}

TEST_CASE("littlefs in the VFS: files kept across mounts, stale after an unmount") {
        Flash f("/lfs");
        put(f.at("/kept"), "kept");
        FileHandle::Ptr open1 = open(f.at("/kept"), Vfs::Read);
        auto [dir, derr] = Vfs::openDirectory(f.at(""), Root);
        REQUIRE(derr.isOk());
        CHECK(Vfs::unmount("/lfs").isOk());
        uint8_t c = 0;
        CHECK(open1->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::NoMedia);
        CHECK(dir.next().error() == Error::NoMedia);
        open1.reset(); // closed: only freed
        dir.close();
        REQUIRE(Vfs::mount("/lfs", f.device).isOk());
        CHECK(get(f.at("/kept")) == "kept");
        // The first mount of a flash with no littlefs on it.
        RamFlashDevice blank;
        REQUIRE(blank.allocate(BlockBytes, 16, bmds::MemSpace::Psram).isOk());
        CHECK(Vfs::mount("/blank", blank) == Error::NoFilesystem);
}

TEST_CASE("littlefs in the VFS: a full flash") {
        Flash                f("/lfs", 32);
        FileHandle::Ptr      h = open(f.at("/fill"), Vfs::Write | Vfs::Create);
        std::vector<uint8_t> chunk(8192, 0x5a);
        uint64_t             written = 0;
        Error                err;
        while (err.isOk()) {
                auto [n, e] = h->write(ByteSpan(chunk.data(), chunk.size()), WaitForever);
                err = e;
                if (e.isOk()) written += n;
        }
        CHECK(err == Error::NoSpace);
        CHECK(written < 32u * BlockBytes);
        CHECK(written > 16u * BlockBytes);
        h.reset();
        // Still usable: a file removed makes room.
        CHECK(Vfs::remove(f.at("/fill"), Root).isOk());
        put(f.at("/after"), "room again");
        CHECK(get(f.at("/after")) == "room again");
}

TEST_CASE("FlashQueue: in order, overlaid, around the ring") {
        FlashQueue q;
        REQUIRE(q.init(4096, bmds::MemSpace::Psram).isOk());
        CHECK(q.init(4096, bmds::MemSpace::Psram) == Error::InvalidState);
        std::vector<uint8_t> flash(8192, 0x00); // the flash before: anything
        const uint8_t        a[3] = {0x0f, 0xf0, 0x55};
        REQUIRE(q.tryErase(4096, 4096).isOk());
        REQUIRE(q.tryProgram(4097, ByteSpan(a, 3)).isOk());
        REQUIRE(q.tryProgram(8190, ByteSpan(a, 2)).isOk());
        CHECK(q.count() == 3);
        std::vector<uint8_t> view(flash.begin() + 4094, flash.begin() + 4102);
        q.overlay(4094, WritableByteSpan(view.data(), view.size()));
        CHECK(view == std::vector<uint8_t>{0x00, 0x00, 0xff, 0x0f, 0xf0, 0x55, 0xff, 0xff});
        FlashQueue::Op op;
        REQUIRE(q.front(op));
        CHECK(op.kind == FlashQueue::Kind::Erase);
        CHECK(op.address == 4096);
        q.pop();
        REQUIRE(q.front(op));
        CHECK(op.kind == FlashQueue::Kind::Program);
        CHECK(op.length == 3);
        CHECK(std::memcmp(op.data, a, 3) == 0);
        CHECK(q.tryProgram(0, ByteSpan(a, 0)) == Error::Invalid);
        std::vector<uint8_t> tooBig(q.maxProgram() + 1);
        CHECK(q.tryProgram(0, ByteSpan(tooBig.data(), tooBig.size())) == Error::Invalid);
        // Around the ring many times: whatever fits goes in, in order.
        std::mt19937         rng(3);
        std::vector<uint8_t> model(8192, 0xff), disk(8192, 0xff);
        while (!q.isEmpty()) q.pop();
        for (int i = 0; i < 20000; ++i) {
                if (rng() % 3 != 0 || q.isEmpty()) {
                        std::vector<uint8_t> data(1 + rng() % 900);
                        for (auto &b : data) b = static_cast<uint8_t>(rng());
                        const uint32_t at = rng() % (8192 - uint32_t(data.size()));
                        if (q.tryProgram(at, ByteSpan(data.data(), data.size())).isOk())
                                for (size_t k = 0; k < data.size(); ++k) model[at + k] &= data[k];
                        if (rng() % 50 == 0 && q.tryErase(0, 4096).isOk())
                                std::fill(model.begin(), model.begin() + 4096, 0xff);
                } else {
                        REQUIRE(q.front(op));
                        if (op.kind == FlashQueue::Kind::Erase) {
                                std::fill(disk.begin() + op.address, disk.begin() + op.address + op.length, 0xff);
                        } else {
                                for (size_t k = 0; k < op.length; ++k) disk[op.address + k] &= op.data[k];
                        }
                        q.pop();
                }
                if (i % 997 == 0) {
                        std::vector<uint8_t> seen = disk;
                        q.overlay(0, WritableByteSpan(seen.data(), seen.size()));
                        REQUIRE(seen == model);
                }
        }
}

namespace {

        // A file's version: a header (file, version, size), then bytes from both.
        std::string versionOf(int file, int version, size_t size) {
                std::string    out(size, '\0');
                const uint32_t header[3] = {uint32_t(file), uint32_t(version), uint32_t(size)};
                std::memcpy(out.data(), header, sizeof(header));
                std::mt19937 gen(uint32_t(file * 7919 + version));
                for (size_t i = sizeof(header); i < size; ++i) out[i] = static_cast<char>(gen());
                return out;
        }

        // True if @p text is a whole version of file @p file (or empty: made, not yet written).
        bool isWholeVersion(int file, const std::string &text) {
                if (text.empty()) return true;
                if (text.size() < 12) return false;
                uint32_t header[3];
                std::memcpy(header, text.data(), sizeof(header));
                return header[0] == uint32_t(file) && header[2] == text.size() &&
                       text == versionOf(file, int(header[1]), text.size());
        }

} // namespace

TEST_CASE("littlefs through the write queue: reads see it, a cut keeps every file whole") {
        // Small ring: it wraps and fills often, so writers wait on it as on the board.
        Flash          f("/lfs", 128, 16 * 1024);
        RamFlashDevice cut;
        REQUIRE(cut.allocate(BlockBytes, 128, bmds::MemSpace::Psram).isOk());
        std::vector<uint8_t> image(size_t(128) * BlockBytes);
        constexpr int        Files = 6;
        int                  latest[Files];
        std::fill(latest, latest + Files, -1);
        std::mt19937 rng(5);
        int          version = 0;
        int          cuts = 0;
        int          cutsLosingWrites = 0; // cuts that dropped queued writes
        for (int step = 0; step < 300; ++step) {
                const int    file = int(rng() % Files);
                const Path   name = f.at("/f" + std::to_string(file));
                const int    v = version++;
                const size_t size = rng() % 3 == 0 ? 2000 + rng() % 9000 : 12 + rng() % 400;
                if (rng() % 4 == 0) {
                        // Replaced by renaming a new file over it.
                        put(f.at("/tmp"), versionOf(file, v, size));
                        (void)Vfs::remove(name, Root);
                        REQUIRE(Vfs::rename(f.at("/tmp"), name, Root).isOk());
                } else {
                        put(name, versionOf(file, v, size));
                }
                latest[file] = v;
                // Reads see everything, done or still queued.
                for (int k = 0; k < Files; ++k) {
                        if (latest[k] < 0) continue;
                        const std::string text = get(f.at("/f" + std::to_string(k)));
                        uint32_t          header[3];
                        REQUIRE(text.size() >= 12);
                        std::memcpy(header, text.data(), sizeof(header));
                        CHECK(int(header[1]) == latest[k]);
                        CHECK(isWholeVersion(k, text));
                }
                // The worker gets some of it done.
                f.device.drain(rng() % 6);
                if (step % 7 != 0) continue;
                // A power cut now: the flash without the queue mounts, and every file is whole.
                ++cuts;
                if (f.device.queued() != 0) ++cutsLosingWrites;
                REQUIRE(f.device.snapshot(WritableByteSpan(image.data(), image.size())).isOk());
                REQUIRE(cut.load(ByteSpan(image.data(), image.size())).isOk());
                REQUIRE(Vfs::mount("/cut", cut).isOk());
                for (int k = 0; k < Files; ++k) {
                        auto [h, err] = Vfs::open(path("/cut/f" + std::to_string(k)), Vfs::Read, Root);
                        if (err == Error::NotExist) continue;
                        REQUIRE(err.isOk());
                        h.reset();
                        CHECK(isWholeVersion(k, get(path("/cut/f" + std::to_string(k)))));
                }
                CHECK(Vfs::unmount("/cut").isOk());
        }
        CHECK(cuts > 30);
        CHECK(cutsLosingWrites > cuts / 2);
        // fsync waits for the queue.
        {
                FileHandle::Ptr h = open(f.at("/synced"), Vfs::Write | Vfs::Create);
                CHECK(h->write(bmds::StringView("durable"), WaitForever).error().isOk());
                CHECK(h->stream().flush(WaitForever).isOk());
                CHECK(f.device.queued() == 0);
        }
}
