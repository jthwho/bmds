/**
 * @file      tmpfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tmpfs in the VFS (devplan/storage.md §15): what the FAT and littlefs
 * tests ask, and what only it does (holes that cost nothing, files removed
 * while open, the limit, every byte given back).
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <bmds/clock.h>
#include <bmds/directorywatch.h>
#include <bmds/vfs.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileInfo;
using bmds::FileType;
using bmds::Stream;
using bmds::Timestamp;
using bmds::WaitForever;
using bmds::WritableByteSpan;
namespace Vfs = bmds::Vfs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        constexpr uint64_t Limit = 1024 * 1024;

        // A path the VFS (StringView) and the helpers (std::string) both take.
        struct Path {
                        std::string text;
                        operator bmds::StringView() const { return bmds::StringView(text.data(), text.size()); }
                        operator const std::string &() const { return text; }
        };

        // A tmpfs at /t for one test; unmounted with the object.
        struct Tmp {
                        explicit Tmp(uint64_t limit = Limit) {
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(Vfs::mountTmpfs("/t", limit).isOk());
                        }

                        ~Tmp() { (void)Vfs::unmount("/t"); }

                        static Path at(const std::string &name) { return Path{"/t" + name}; }
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

        uint64_t freeBytes() {
                return Vfs::statfs("/t", Root).value().freeBytes;
        }

} // namespace

TEST_CASE("tmpfs: files and directories") {
        const size_t base = HostMemory::liveAllocations();
        {
                Tmp            t;
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(bmds::Span<Vfs::MountInfo>(mounts));
                bool           found = false;
                for (size_t i = 0; i < n; ++i) {
                        if (std::string(mounts[i].path) != "/t") continue;
                        found = true;
                        CHECK(std::string(mounts[i].type) == "tmpfs");
                        CHECK(std::string(mounts[i].device).empty());
                }
                CHECK(found);
                const bmds::FsInfo fs = Vfs::statfs("/t", Root).value();
                CHECK(std::string(fs.type) == "tmpfs");
                CHECK(fs.totalBytes == Limit);
                CHECK(fs.freeBytes < Limit);
                CHECK(fs.freeBytes > Limit - 256);
                CHECK(Vfs::mountTmpfs("/t", Limit) == Error::Exists);
                CHECK(Vfs::mountTmpfs("/t/in", Limit) == Error::Invalid);

                put(t.at("/a.txt"), "hello");
                CHECK(get(t.at("/a.txt")) == "hello");
                put(t.at("/a.txt"), " world", Vfs::Write | Vfs::Append);
                CHECK(get(t.at("/a.txt")) == "hello world");
                CHECK(Vfs::stat(t.at("/a.txt"), Root).value().size == 11);
                CHECK(Vfs::open(t.at("/a.txt"), Vfs::Write | Vfs::Create | Vfs::Exclusive, Root).error() ==
                      Error::Exists);
                CHECK(Vfs::open(t.at("/none"), Vfs::Read, Root).error() == Error::NotExist);
                // Files across the page size, written in odd pieces.
                for (size_t size : {size_t(63), size_t(64), size_t(4095), size_t(4096), size_t(4097), size_t(30000)}) {
                        std::string big;
                        for (size_t i = 0; i < size; ++i) big += char('a' + i % 23);
                        FileHandle::Ptr h = open(t.at("/big"), Vfs::Write | Vfs::Create | Vfs::Truncate);
                        for (size_t at = 0; at < size; at += 777)
                                REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(big.data()) + at,
                                                          std::min<size_t>(777, size - at)),
                                                 WaitForever)
                                                .error()
                                                .isOk());
                        h.reset();
                        CHECK(get(t.at("/big")) == big);
                }

                CHECK(Vfs::mkdir(t.at("/dir"), Root).isOk());
                CHECK(Vfs::mkdir(t.at("/dir"), Root) == Error::Exists);
                CHECK(Vfs::mkdir(t.at("/no/dir"), Root) == Error::NotExist);
                CHECK(Vfs::open(t.at("/dir"), Vfs::Read, Root).error() == Error::IsDir);
                CHECK(Vfs::stat(t.at("/a.txt/x"), Root).error() == Error::NotExist);
                CHECK(Vfs::open(t.at("/a.txt/x"), Vfs::Write | Vfs::Create, Root).error() == Error::NotExist);
                put(t.at("/dir/b"), "b");
                CHECK(Vfs::mkdir(t.at("/dir/sub"), Root).isOk());
                CHECK(list(t.at("")) == std::vector<std::string>{"a.txt", "big", "dir/"});
                CHECK(list(t.at("/dir")) == std::vector<std::string>{"b", "sub/"});
                CHECK(Vfs::openDirectory(t.at("/a.txt"), Root).error() == Error::NotDir);
                CHECK(Vfs::rmdir(t.at("/dir"), Root) == Error::NotEmpty);
                CHECK(Vfs::remove(t.at("/dir"), Root) == Error::IsDir);
                CHECK(Vfs::rmdir(t.at("/a.txt"), Root) == Error::NotDir);
                // Renames don't replace (as on FAT), move between directories, never into themselves.
                CHECK(Vfs::rename(t.at("/a.txt"), t.at("/big"), Root) == Error::Exists);
                CHECK(Vfs::rename(t.at("/a.txt"), t.at("/dir/sub/a"), Root).isOk());
                CHECK(get(t.at("/dir/sub/a")) == "hello world");
                CHECK(Vfs::rename(t.at("/dir"), t.at("/dir/sub/dir"), Root) == Error::Invalid);
                CHECK(Vfs::rename(t.at("/gone"), t.at("/x"), Root) == Error::NotExist);
                // Long names (past the node's own room), and back.
                const std::string longName = "/" + std::string(200, 'L');
                CHECK(Vfs::rename(t.at("/dir/sub/a"), t.at(longName), Root).isOk());
                CHECK(get(t.at(longName)) == "hello world");
                CHECK(Vfs::open(t.at("/" + std::string(256, 'x')), Vfs::Write | Vfs::Create, Root).error() ==
                      Error::Invalid);
                CHECK(Vfs::rename(t.at(longName), t.at("/short"), Root).isOk());
                CHECK(Vfs::remove(t.at("/short"), Root).isOk());
                CHECK(Vfs::rmdir(t.at("/dir/sub"), Root).isOk());
                CHECK(Vfs::remove(t.at("/dir/b"), Root).isOk());
                CHECK(Vfs::rmdir(t.at("/dir"), Root).isOk());
                CHECK(list(t.at("")) == std::vector<std::string>{"big"});
                // Names keep their case, and are case-sensitive.
                put(t.at("/Case"), "C");
                put(t.at("/case"), "c");
                CHECK(get(t.at("/Case")) == "C");
                CHECK(get(t.at("/case")) == "c");
                CHECK(Vfs::copyFile(t.at("/Case"), t.at("/copy"), Root).isOk());
                CHECK(get(t.at("/copy")) == "C");
                // A tree left in place goes with the unmount.
                CHECK(Vfs::mkdir(t.at("/x"), Root).isOk());
                CHECK(Vfs::mkdir(t.at("/x/y"), Root).isOk());
                put(t.at("/x/y/z"), std::string(10000, 'z'));
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("tmpfs: holes, truncation, and the space they take") {
        Tmp            t;
        const uint64_t empty = freeBytes();
        {
                FileHandle::Ptr h = open(t.at("/h.bin"), Vfs::Read | Vfs::Write | Vfs::Create);
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
                // Open for writing: others may open it too, and read what it writes.
                FileHandle::Ptr other = open(t.at("/h.bin"), Vfs::Read);
                CHECK(Vfs::open(t.at("/h.bin"), Vfs::Write, Root).error().isOk());
        }
        CHECK(get(t.at("/h.bin")) == std::string("ab") + std::string(8, '\0'));
        // What a truncate cut doesn't come back when the file grows again: within a block, and a page.
        put(t.at("/g.bin"), std::string(9000, 'g'));
        {
                FileHandle::Ptr h = open(t.at("/g.bin"), Vfs::Write);
                CHECK(h->stream().truncate(4100).isOk());
                CHECK(h->stream().truncate(9000).isOk());
                CHECK(h->stream().truncate(5).isOk());
                CHECK(h->stream().seek(8000, Stream::SeekFrom::Start).isOk());
                CHECK(h->write(bmds::StringView("y"), WaitForever).error().isOk());
        }
        const std::string g = get(t.at("/g.bin"));
        CHECK(g.size() == 8001);
        CHECK(g.substr(0, 5) == "ggggg");
        CHECK(g.find_first_not_of('\0', 5) == 8000);
        put(t.at("/s.bin"), std::string(100, 's'));
        {
                FileHandle::Ptr h = open(t.at("/s.bin"), Vfs::Write);
                CHECK(h->stream().truncate(10).isOk());
                CHECK(h->stream().truncate(50).isOk());
        }
        CHECK(get(t.at("/s.bin")) == std::string(10, 's') + std::string(40, '\0'));
        CHECK(Vfs::remove(t.at("/g.bin"), Root).isOk());
        CHECK(Vfs::remove(t.at("/s.bin"), Root).isOk());
        CHECK(Vfs::remove(t.at("/h.bin"), Root).isOk());
        CHECK(freeBytes() == empty);
        // A hole costs nothing: a byte at 512 KB takes a page and a table, not 512 KB.
        {
                FileHandle::Ptr h = open(t.at("/sparse"), Vfs::Write | Vfs::Create);
                CHECK(h->stream().seek(512 * 1024, Stream::SeekFrom::Start).isOk());
                CHECK(h->write(bmds::StringView("!"), WaitForever).error().isOk());
                CHECK(h->stream().truncate(800 * 1024).isOk());
        }
        CHECK(Vfs::stat(t.at("/sparse"), Root).value().size == 800u * 1024);
        CHECK(empty - freeBytes() < 8192);
        {
                FileHandle::Ptr      h = open(t.at("/sparse"), Vfs::Read);
                std::vector<uint8_t> buf(4096, 0xee);
                CHECK(h->stream().seek(512 * 1024 - 2, Stream::SeekFrom::Start).isOk());
                CHECK(h->read(WritableByteSpan(buf.data(), 4), WaitForever).value() == 4);
                CHECK(buf[0] == 0);
                CHECK(buf[1] == 0);
                CHECK(buf[2] == '!');
                CHECK(buf[3] == 0);
        }
        CHECK(Vfs::remove(t.at("/sparse"), Root).isOk());
        CHECK(freeBytes() == empty);
}

TEST_CASE("tmpfs: removed while open, listings while removing") {
        Tmp            t;
        const uint64_t empty = freeBytes();
        put(t.at("/f"), "still here");
        {
                FileHandle::Ptr h = open(t.at("/f"), Vfs::Read | Vfs::Write);
                CHECK(Vfs::remove(t.at("/f"), Root).isOk());
                CHECK(Vfs::stat(t.at("/f"), Root).error() == Error::NotExist);
                CHECK(list(t.at("")).empty());
                // The name is free again; the open file is another.
                put(t.at("/f"), "new");
                CHECK(h->write(bmds::StringView("!"), WaitForever).error().isOk());
                CHECK(h->stream().seek(0, Stream::SeekFrom::Start).isOk());
                uint8_t buf[32];
                auto [n, err] = h->read(WritableByteSpan(buf), WaitForever);
                CHECK(err.isOk());
                CHECK(std::string(reinterpret_cast<char *>(buf), n) == "!till here");
                CHECK(get(t.at("/f")) == "new");
                CHECK(Vfs::remove(t.at("/f"), Root).isOk());
                CHECK(freeBytes() < empty);
        }
        CHECK(freeBytes() == empty);
        // rm -r's way: list, removing each entry as it is listed (and some not yet listed).
        CHECK(Vfs::mkdir(t.at("/d"), Root).isOk());
        for (int i = 0; i < 20; ++i) put(t.at("/d/" + std::to_string(i)), "x");
        {
                auto [dir, err] = Vfs::openDirectory(t.at("/d"), Root);
                REQUIRE(err.isOk());
                std::vector<std::string> seen;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) break;
                        REQUIRE(nerr.isOk());
                        const std::string name(info.name.cStr());
                        seen.push_back(name);
                        CHECK(Vfs::remove(t.at("/d/" + name), Root).isOk());
                        // The next one too, every fifth: the listing goes on past it.
                        if (seen.size() % 5 == 0)
                                (void)Vfs::remove(t.at("/d/" + std::to_string(std::stoi(name) + 1)), Root);
                }
                CHECK(seen.size() == 17);
                // The directory itself, while listed: gone when the listing closes.
                CHECK(Vfs::rmdir(t.at("/d"), Root).isOk());
                CHECK(dir.next().error() == Error::EndOfFile);
        }
        CHECK(freeBytes() == empty);
}

TEST_CASE("tmpfs: links, times, identity and watches") {
        Tmp t;
        CHECK(Vfs::mkdir(t.at("/bin"), Root).isOk());
        CHECK(Vfs::mkdir(t.at("/data"), Root).isOk());
        put(t.at("/bin/busybox"), "BB");
        put(t.at("/data/notes.txt"), "notes");
        CHECK(Vfs::symlink("busybox", t.at("/bin/awk"), Root).isOk());
        CHECK(Vfs::symlink("/t/data", t.at("/d"), Root).isOk());
        CHECK(Vfs::symlink("busybox", t.at("/bin/awk"), Root) == Error::Exists);
        CHECK(Vfs::symlink("x", t.at("/none/x"), Root) == Error::NotExist);
        CHECK(get(t.at("/bin/awk")) == "BB");
        const FileInfo awk = Vfs::lstat(t.at("/bin/awk"), Root).value();
        CHECK(awk.type == FileType::Link);
        CHECK(Vfs::readLink(t.at("/bin/awk"), Root).value() == "busybox");
        CHECK(Vfs::readLink(t.at("/bin/busybox"), Root).error() == Error::Invalid);
        CHECK(get(t.at("/d/notes.txt")) == "notes");
        put(t.at("/d/new.txt"), "through the link");
        CHECK(get(t.at("/data/new.txt")) == "through the link");
        CHECK(list(t.at("/bin")) == std::vector<std::string>{"awk@", "busybox"});
        put(t.at("/bin/awk"), "short");
        CHECK(get(t.at("/bin/busybox")) == "short");
        CHECK(Vfs::lstat(t.at("/bin/awk"), Root).value().type == FileType::Link);

        const int64_t when = bmds::Calendar::daysFromCivil(2026, 6, 15) * 86400 + 12 * 3600;
        REQUIRE(bmds::Clock::set(Timestamp::fromSeconds(when), bmds::ClockSource::Manual).isOk());
        put(t.at("/stamped"), "x");
        const int64_t stamped = Vfs::stat(t.at("/stamped"), Root).value().modified.seconds();
        CHECK(stamped >= when);
        CHECK(stamped <= when + 2);
        const Timestamp old{Timestamp::fromSeconds(946684799).ns + 123456789};
        CHECK(Vfs::setModified(t.at("/stamped"), old, Root).isOk());
        CHECK(Vfs::stat(t.at("/stamped"), Root).value().modified.ns == old.ns);
        CHECK(Vfs::setModified(t.at("/data"), old, Root).isOk());
        CHECK(Vfs::stat(t.at("/data"), Root).value().modified.ns == old.ns);
        const FileInfo a = Vfs::stat(t.at("/data/notes.txt"), Root).value();
        CHECK(a.id != 0);
        CHECK(open(t.at("/data/notes.txt"), Vfs::Read)->id() == a.id);

        bmds::DirectoryWatch watch;
        CHECK(watch.watch(t.at("/data"), Root).isOk());
        put(t.at("/data/file"), "x");
        CHECK(watch.takeChanges() == bmds::DirectoryWatch::Contents);
        CHECK(Vfs::rename(t.at("/data/file"), t.at("/moved"), Root).isOk());
        CHECK(watch.takeChanges() == bmds::DirectoryWatch::Entries);
}

TEST_CASE("tmpfs: the limit, and stale files after an unmount") {
        const size_t base = HostMemory::liveAllocations();
        {
                Tmp                  t(64 * 1024);
                FileHandle::Ptr      h = open(t.at("/fill"), Vfs::Write | Vfs::Create);
                std::vector<uint8_t> chunk(3000, 0x5a);
                uint64_t             written = 0;
                Error                err;
                while (err.isOk()) {
                        auto [n, e] = h->write(ByteSpan(chunk.data(), chunk.size()), WaitForever);
                        err = e;
                        if (e.isOk()) written += n;
                }
                CHECK(err == Error::NoSpace);
                CHECK(written <= 64u * 1024);
                CHECK(written > 56u * 1024);
                // A higher limit makes room.
                CHECK(Vfs::setTmpfsLimit("/t", 128 * 1024).isOk());
                CHECK(h->write(ByteSpan(chunk.data(), chunk.size()), WaitForever).value() == chunk.size());
                CHECK(Vfs::setTmpfsLimit("/nowhere", 1) == Error::NotExist);
                h.reset();
                CHECK(Vfs::remove(t.at("/fill"), Root).isOk());
                put(t.at("/after"), "room again");

                FileHandle::Ptr kept = open(t.at("/after"), Vfs::Read);
                auto [dir, derr] = Vfs::openDirectory(t.at(""), Root);
                REQUIRE(derr.isOk());
                CHECK(Vfs::unmount("/t").isOk());
                uint8_t c = 0;
                CHECK(kept->read(WritableByteSpan(&c, 1), WaitForever).error() == Error::NoMedia);
                CHECK(dir.next().error() == Error::NoMedia);
                kept.reset();
                dir.close();
                // Mounted again: empty.
                REQUIRE(Vfs::mountTmpfs("/t", Limit).isOk());
                CHECK(list(t.at("")).empty());
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("tmpfs: random writes match a model") {
        Tmp                  t;
        std::mt19937         rng(1234);
        std::string          model;
        FileHandle::Ptr      h = open(t.at("/r"), Vfs::Read | Vfs::Write | Vfs::Create);
        std::vector<uint8_t> buf;
        for (int i = 0; i < 400; ++i) {
                const uint32_t what = rng() % 10;
                if (what < 6) {
                        const size_t at = rng() % 20000;
                        const size_t n = 1 + rng() % 6000;
                        buf.resize(n);
                        for (auto &b : buf) b = uint8_t(rng());
                        REQUIRE(h->stream().seek(int64_t(at), Stream::SeekFrom::Start).isOk());
                        REQUIRE(h->write(ByteSpan(buf.data(), n), WaitForever).value() == n);
                        if (model.size() < at + n) model.resize(at + n, '\0');
                        std::memcpy(model.data() + at, buf.data(), n);
                } else if (what < 8) {
                        const size_t size = rng() % 24000;
                        REQUIRE(h->stream().truncate(size).isOk());
                        model.resize(size, '\0');
                } else {
                        const size_t at = rng() % 24000;
                        const size_t n = 1 + rng() % 9000;
                        buf.assign(n, 0xcc);
                        REQUIRE(h->stream().seek(int64_t(at), Stream::SeekFrom::Start).isOk());
                        auto [got, err] = h->read(WritableByteSpan(buf.data(), n), WaitForever);
                        if (at >= model.size()) {
                                CHECK(err == Error::EndOfFile);
                        } else {
                                REQUIRE(err.isOk());
                                CHECK(got == std::min(n, model.size() - at));
                                CHECK(std::memcmp(buf.data(), model.data() + at, got) == 0);
                        }
                }
        }
        h.reset();
        CHECK(get(t.at("/r")) == model);
}
