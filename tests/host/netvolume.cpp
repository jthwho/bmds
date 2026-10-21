/**
 * @file      netvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Volumes that lock themselves (devplan/netfs.md step 1): a network
 * filesystem's calls run without the storage lock, so one waiting on its
 * network holds no other file operation; it stays alive for a call under way
 * or a directory open though it is unmounted meanwhile.
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <bmds/vfs.h>
#include "../../components/bmds_fs/src/fslayer.h"
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileInfo;
using bmds::FileType;
using bmds::FsInfo;
using bmds::Result;
using bmds::String;
using bmds::StringView;
using bmds::Timestamp;
using bmds::Volume;
using bmds::WaitForever;
namespace Vfs = bmds::Vfs;
namespace Fs = bmds::Fs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        // A volume over a "network": every stat can be held until let go; its calls note
        // whether the storage lock was free while they ran.
        class TestNetVolume final : public Volume {
                public:
                        std::atomic<int>        refs{1}; // the mount's
                        std::atomic<bool>       freed{false};
                        std::atomic<int>        lockedCalls{0}; // calls made with the storage lock held
                        std::atomic<int>        calls{0};
                        std::mutex              m;
                        std::condition_variable cv;
                        bool                    hold = false;    // stats wait while set
                        bool                    waiting = false; // a stat is waiting

                        void release() override {
                                if (--refs == 0) freed = true;
                        }
                        bool               locksItself() const override { return true; }
                        void               retain() override { ++refs; }
                        bmds::BlockDevice *device() const override { return nullptr; }
                        const char        *typeName() const override { return "testnet"; }

                        Result<FsInfo> info() override {
                                note();
                                FsInfo i;
                                i.type = "testnet";
                                i.totalBytes = 1000;
                                return bmds::makeResult(i);
                        }

                        Error stat(StringView path, FileInfo &info, bool &noPath) override {
                                note();
                                noPath = false;
                                {
                                        std::unique_lock<std::mutex> l(m);
                                        waiting = true;
                                        cv.notify_all();
                                        cv.wait(l, [&] { return !hold; });
                                        waiting = false;
                                }
                                if (path != "/file" && path != "/dir") return Error::NotExist;
                                (void)info.name.assign(path.mid(1));
                                info.type = path == "/dir" ? FileType::Directory : FileType::File;
                                info.size = 5;
                                return Error::Ok;
                        }

                        Result<FileHandle::Ptr> open(StringView, uint8_t, const bmds::FileIdentity &, bool &noPath,
                                                     String *) override {
                                note();
                                noPath = false;
                                return bmds::makeError<FileHandle::Ptr>(Error::NotSupported);
                        }
                        Result<Reader *> openReader(StringView) override {
                                note();
                                return bmds::makeError<Reader *>(Error::NotSupported);
                        }
                        void  closeReader(Reader *) override {}
                        Error mkdir(StringView, bool &noPath) override {
                                note();
                                noPath = false;
                                return Error::Ok;
                        }
                        Error rmdir(StringView) override {
                                note();
                                return Error::Ok;
                        }
                        Error remove(StringView) override {
                                note();
                                return Error::Ok;
                        }
                        Error rename(StringView, StringView, Vfs::RenameMode, bool &noPath) override {
                                note();
                                noPath = false;
                                return Error::Ok;
                        }
                        Error setModified(StringView, Timestamp) override {
                                note();
                                return Error::Ok;
                        }
                        Error writeLink(StringView, ByteSpan, bool &noPath) override {
                                note();
                                noPath = false;
                                return Error::NotSupported;
                        }

                        // One entry, "file".
                        class OneCursor final : public Cursor {
                                public:
                                        TestNetVolume *owner = nullptr;
                                        bool           given = false;
                                        Error          next(FileInfo &info) override {
                                                owner->note();
                                                if (given) return Error::EndOfFile;
                                                given = true;
                                                (void)info.name.assign("file");
                                                info.type = FileType::File;
                                                return Error::Ok;
                                        }
                        };
                        OneCursor cursor;

                        Result<Cursor *> openDirectory(StringView, bool &noPath) override {
                                note();
                                noPath = false;
                                cursor.owner = this;
                                cursor.given = false;
                                return bmds::makeResult<Cursor *>(&cursor);
                        }
                        void closeDirectory(Cursor *) override { note(); }

                        // Was the storage lock free? Taken and let go at once, from another thread
                        // (the calling one may hold it: the mutex isn't recursive).
                        void note() {
                                ++calls;
                                std::atomic<bool> got{false};
                                std::thread       t([&] {
                                        if (Fs::lock().lock(50).isOk()) {
                                                got = true;
                                                Fs::lock().unlock();
                                        }
                                });
                                t.join();
                                if (!got) ++lockedCalls;
                        }

                        void setHold(bool on) {
                                std::lock_guard<std::mutex> l(m);
                                hold = on;
                                cv.notify_all();
                        }

                        void waitUntilWaiting() {
                                std::unique_lock<std::mutex> l(m);
                                cv.wait(l, [&] { return waiting; });
                        }
        };

} // namespace

TEST_CASE("netfs: a volume that locks itself is called without the storage lock") {
        REQUIRE(Vfs::init().isOk());
        auto *v = new TestNetVolume();
        REQUIRE(Fs::mountVolume("/net", *v).isOk());
        CHECK(Vfs::isMounted("/net"));

        // Every kind of call, none with the lock held.
        CHECK(Vfs::stat("/net/file", Root).isOk());
        CHECK(Vfs::stat("/net/nothing", Root).error() == Error::NotExist);
        CHECK(Vfs::mkdir("/net/new", Root) == Error::Ok);
        CHECK(Vfs::rmdir("/net/dir", Root) == Error::Ok);
        CHECK(Vfs::remove("/net/file", Root) == Error::Ok);
        CHECK(Vfs::rename("/net/file", "/net/other", Vfs::RenameMode::Replace, Root) == Error::Ok);
        CHECK(Vfs::setModified("/net/file", Timestamp(), Root) == Error::Ok);
        CHECK(Vfs::statfs("/net/file", Root).isOk());
        CHECK(Vfs::map("/net/file", Root).error() == Error::NotSupported); // never in place
        {
                auto [dir, err] = Vfs::openDirectory("/net/dir", Root);
                REQUIRE(err.isOk());
                CHECK(v->refs.load() == 2); // the mount's and the directory's
                auto [entry, eerr] = dir.next();
                CHECK(eerr.isOk());
                CHECK(entry.name.view() == "file");
                CHECK(dir.next().error() == Error::EndOfFile);
        }
        CHECK(v->refs.load() == 1);
        CHECK(v->calls.load() > 10);
        CHECK(v->lockedCalls.load() == 0);

        // Listed as a network filesystem; no trash there.
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(bmds::Span<Vfs::MountInfo>(mounts, Vfs::MaxMounts));
        bool           seen = false;
        for (size_t i = 0; i < n; ++i)
                if (StringView(mounts[i].path) == "/net")
                        seen = mounts[i].network && StringView(mounts[i].type) == "testnet";
        CHECK(seen);

        CHECK(Vfs::unmount("/net") == Error::Ok);
        CHECK(v->freed.load());
        delete v;
}

TEST_CASE("netfs: a call waiting on the network holds no other file operation") {
        REQUIRE(Vfs::init().isOk());
        REQUIRE(Vfs::mountTmpfs("/t", 1 << 20).isOk());
        auto *v = new TestNetVolume();
        REQUIRE(Fs::mountVolume("/net", *v).isOk());

        // A stat held on the "network"...
        v->setHold(true);
        std::atomic<bool> done{false};
        Error             result = Error::Invalid;
        std::thread       caller([&] {
                result = Vfs::stat("/net/file", Root).error();
                done = true;
        });
        v->waitUntilWaiting();
        // ...while the rest go on: a file made, written, read, listed, removed in /t.
        {
                auto [h, err] = Vfs::open("/t/a", Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                REQUIRE(err.isOk());
                CHECK(h->write(ByteSpan(reinterpret_cast<const uint8_t *>("hello"), 5), WaitForever).error().isOk());
        }
        CHECK(Vfs::stat("/t/a", Root).value().size == 5);
        CHECK(Vfs::remove("/t/a", Root) == Error::Ok);
        CHECK(!done.load());

        // Unmounted while the call waits: the unmount returns, the volume lives on for the call.
        CHECK(Vfs::unmount("/net") == Error::Ok);
        CHECK(!Vfs::isMounted("/net"));
        CHECK(!v->freed.load());
        CHECK(Vfs::stat("/net/file", Root).error() == Error::NotExist); // gone for new calls
        v->setHold(false);
        caller.join();
        CHECK(result == Error::Ok); // the call under way finished against it
        CHECK(v->freed.load());     // and the last reference went with it
        delete v;
        CHECK(Vfs::unmount("/t") == Error::Ok);
}

TEST_CASE("netfs: a mount refused gives its reference back") {
        REQUIRE(Vfs::init().isOk());
        auto *v = new TestNetVolume();
        CHECK(Fs::mountVolume("/dev/net", *v) == Error::Invalid);
        CHECK(v->freed.load());
        delete v;
}
