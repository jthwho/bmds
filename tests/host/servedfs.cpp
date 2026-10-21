/**
 * @file      servedfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Filesystems served by programs (bmds/servedfs.h, devplan/netfs.md step 2):
 * a server thread here keeps a tree in memory and answers the requests, as
 * a program does through its server fd; the VFS's calls go through it.
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <bmds/servedfs.h>
#include <bmds/vfs.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::FileType;
using bmds::StringView;
using bmds::WaitForever;
using bmds::WritableByteSpan;
namespace Vfs = bmds::Vfs;
namespace Served = bmds::ServedFs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        // The server's error numbers, as a program's are (a few of BMDS's own).
        constexpr int32_t ENoEnt = -2, EExist = -17, ENotDir = -20, EIsDir = -21, ENotEmpty = -39, ENoSys = -38;

        Error errorOf(int32_t e) {
                switch (e) {
                        case ENoEnt: return Error::NotExist;
                        case EExist: return Error::Exists;
                        case ENotDir: return Error::NotDir;
                        case EIsDir: return Error::IsDir;
                        case ENotEmpty: return Error::NotEmpty;
                        case ENoSys: return Error::NotSupported;
                        default: return Error::IoError;
                }
        }

        // A tree in memory served through the server's end.
        class TestServer {
                public:
                        struct Node {
                                        bool        dir = false;
                                        std::string data;
                                        int64_t     modified = 0;
                        };

                        std::map<std::string, Node>     nodes;
                        std::map<uint64_t, std::string> handles;
                        uint64_t                        nextHandle = 100;
                        std::atomic<bool>               ignore{false}; // requests read, never answered
                        std::atomic<bool>               quit{false};
                        std::string                     mount = "/srv/wake";
                        std::atomic<int>                requests{0};
                        std::atomic<int>                reversed{0}; // answer in batches of this, last first
                        FileHandle::Ptr                 end;
                        std::thread                     thread;
                        uint32_t                        maxData = 4096;

                        TestServer() { nodes["/"] = Node{true, "", 0}; }

                        void start(FileHandle::Ptr server) {
                                end = std::move(server);
                                thread = std::thread([this] { run(); });
                        }

                        // Ends the server: it stops at its next request (one made here to wake it) and
                        // its fd closes as it ends (the mount goes; the request fails with it).
                        void stop() {
                                if (!thread.joinable()) return;
                                end.reset();
                                quit = true;
                                std::thread waker(
                                        [this] { (void)Vfs::stat(StringView(mount.data(), mount.size()), Root); });
                                thread.join();
                                waker.join();
                        }

                        ~TestServer() { stop(); }

                private:
                        void run() {
                                std::vector<uint8_t>              buf(Served::requestMax(maxData));
                                std::vector<std::vector<uint8_t>> batch;
                                FileHandle::Ptr                   h = end; // ours while it runs
                                for (;;) {
                                        auto [n, err] = h->read(WritableByteSpan(buf.data(), buf.size()), WaitForever);
                                        if (err.isError() || quit.load()) break;
                                        ++requests;
                                        if (ignore.load()) continue;
                                        batch.push_back(answer(buf.data(), n));
                                        if (reversed.load() > 1 && int(batch.size()) < reversed.load()) continue;
                                        for (auto it = batch.rbegin(); it != batch.rend(); ++it)
                                                (void)h->write(ByteSpan(it->data(), it->size()), WaitForever);
                                        batch.clear();
                                }
                                return;
                        }

                        static void put(std::vector<uint8_t> &out, const void *p, size_t n) {
                                const auto *b = static_cast<const uint8_t *>(p);
                                out.insert(out.end(), b, b + n);
                        }

                        static void entry(std::vector<uint8_t> &out, const std::string &name, const Node &node) {
                                Served::Entry e;
                                e.type = node.dir ? Served::TypeDirectory : Served::TypeFile;
                                e.bytes = node.data.size();
                                e.modifiedNs = node.modified;
                                e.nameLength = uint32_t(name.size());
                                e.size = uint32_t((sizeof(e) + name.size() + 7) & ~size_t(7));
                                const size_t at = out.size();
                                put(out, &e, sizeof(e));
                                put(out, name.data(), name.size());
                                out.resize(at + e.size, 0);
                        }

                        static std::string parentOf(const std::string &p) {
                                const size_t s = p.rfind('/');
                                return s == 0 ? "/" : p.substr(0, s);
                        }

                        std::vector<uint8_t> answer(const uint8_t *d, size_t n) {
                                Served::Request r;
                                std::memcpy(&r, d, sizeof(r));
                                REQUIRE(r.size == n);
                                const std::string path(reinterpret_cast<const char *>(d + sizeof(r)), r.pathLength);
                                const std::string path2(reinterpret_cast<const char *>(d + sizeof(r) + r.pathLength),
                                                        r.path2Length);
                                const uint8_t    *data = d + sizeof(r) + r.pathLength + r.path2Length;
                                Served::Reply     reply;
                                reply.id = r.id;
                                std::vector<uint8_t> payload;
                                auto                 found = nodes.find(path);
                                switch (r.op) {
                                        case Served::Stat:
                                                if (found == nodes.end())
                                                        reply.error = ENoEnt;
                                                else
                                                        entry(payload, path.substr(path.rfind('/') + 1), found->second);
                                                break;
                                        case Served::Open:
                                                if (found == nodes.end() && (r.flags & Served::OpenCreate) == 0) {
                                                        reply.error = ENoEnt;
                                                } else if (found != nodes.end() && (r.flags & Served::OpenExclusive)) {
                                                        reply.error = EExist;
                                                } else if (found != nodes.end() && found->second.dir) {
                                                        reply.error = EIsDir;
                                                } else {
                                                        Node &node = nodes[path];
                                                        if (r.flags & Served::OpenTruncate) node.data.clear();
                                                        reply.handle = nextHandle++;
                                                        handles[reply.handle] = path;
                                                }
                                                break;
                                        case Served::Read: {
                                                const std::string &data_ = nodes[handles[r.handle]].data;
                                                if (r.offset < data_.size()) {
                                                        const size_t k =
                                                                std::min<size_t>(r.length, data_.size() - r.offset);
                                                        put(payload, data_.data() + r.offset, k);
                                                }
                                                reply.count = uint32_t(payload.size());
                                                break;
                                        }
                                        case Served::Write: {
                                                std::string &data_ = nodes[handles[r.handle]].data;
                                                if (data_.size() < r.offset + r.length)
                                                        data_.resize(r.offset + r.length);
                                                std::memcpy(&data_[r.offset], data, r.length);
                                                reply.count = r.length;
                                                break;
                                        }
                                        case Served::Truncate: nodes[handles[r.handle]].data.resize(r.offset); break;
                                        case Served::Close: handles.erase(r.handle); break;
                                        case Served::ReadDir: {
                                                if (found == nodes.end()) {
                                                        reply.error = ENoEnt;
                                                        break;
                                                }
                                                if (!found->second.dir) {
                                                        reply.error = ENotDir;
                                                        break;
                                                }
                                                uint64_t skip = r.offset;
                                                for (const auto &[p, node] : nodes) {
                                                        if (p == "/" || parentOf(p) != path) continue;
                                                        if (skip > 0) {
                                                                --skip;
                                                                continue;
                                                        }
                                                        std::vector<uint8_t> one;
                                                        entry(one, p.substr(p.rfind('/') + 1), node);
                                                        if (payload.size() + one.size() > r.length) break;
                                                        payload.insert(payload.end(), one.begin(), one.end());
                                                        ++reply.count;
                                                        if (reply.count == 2)
                                                                break; // small batches: the cursor asks again
                                                }
                                                break;
                                        }
                                        case Served::MakeDir:
                                                if (found != nodes.end())
                                                        reply.error = EExist;
                                                else
                                                        nodes[path] = Node{true, "", 0};
                                                break;
                                        case Served::RemoveDir:
                                        case Served::Remove: {
                                                if (found == nodes.end()) {
                                                        reply.error = ENoEnt;
                                                        break;
                                                }
                                                bool children = false;
                                                for (const auto &kv : nodes)
                                                        children |= kv.first != path && parentOf(kv.first) == path;
                                                if (r.op == Served::RemoveDir && children)
                                                        reply.error = ENotEmpty;
                                                else
                                                        nodes.erase(found);
                                                break;
                                        }
                                        case Served::Rename:
                                                if (found == nodes.end()) {
                                                        reply.error = ENoEnt;
                                                } else if (nodes.count(path2) && !(r.flags & Served::RenameReplace)) {
                                                        reply.error = EExist;
                                                } else {
                                                        nodes[path2] = found->second;
                                                        nodes.erase(path);
                                                }
                                                break;
                                        case Served::SetTime:
                                                if (found == nodes.end())
                                                        reply.error = ENoEnt;
                                                else
                                                        found->second.modified = r.timeNs;
                                                break;
                                        case Served::StatFs: {
                                                Served::Info i{1 << 20, 1 << 19};
                                                put(payload, &i, sizeof(i));
                                                break;
                                        }
                                        default: reply.error = ENoSys; break;
                                }
                                reply.size = uint32_t(sizeof(reply) + payload.size());
                                std::vector<uint8_t> out;
                                put(out, &reply, sizeof(reply));
                                out.insert(out.end(), payload.begin(), payload.end());
                                return out;
                        }
        };

        Served::Config configFor(const char *path, uint32_t timeoutMs = 2000, uint32_t attrMs = 1000) {
                Served::Config c;
                c.path = path;
                c.type = "testfs";
                c.maxData = 4096;
                c.timeoutMs = timeoutMs;
                c.attrMs = attrMs;
                c.errorOf = errorOf;
                return c;
        }

        std::string readAll(const std::string &path) {
                auto [h, err] = Vfs::open(StringView(path.data(), path.size()), Vfs::Read, Root);
                REQUIRE(err.isOk());
                std::string out;
                char        buf[1000]; // smaller than a read-ahead: several reads from one buffer
                for (;;) {
                        auto [n, rerr] =
                                h->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                        if (rerr == Error::EndOfFile) break;
                        REQUIRE(rerr.isOk());
                        out.append(buf, n);
                }
                return out;
        }

} // namespace

TEST_CASE("servedfs: files and directories through a server") {
        REQUIRE(Vfs::init().isOk());
        TestServer server;
        {
                auto [end, err] = Served::serve(configFor("/srv"));
                REQUIRE(err.isOk());
                server.start(std::move(end));
        }
        CHECK(Vfs::isMounted("/srv"));
        CHECK(Vfs::mkdir("/srv/docs", Root) == Error::Ok);
        CHECK(Vfs::mkdir("/srv/docs", Root) == Error::Exists);

        // 10 000 bytes in writes of 7 (gathered into 4 KB writes), read back in 1 000s.
        std::string text;
        for (int i = 0; i < 10000; ++i) text.push_back(char('a' + i % 26));
        {
                auto [h, err] = Vfs::open("/srv/docs/a.txt", Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                REQUIRE(err.isOk());
                const int before = server.requests.load();
                for (size_t at = 0; at < text.size(); at += 7) {
                        const size_t n = std::min<size_t>(7, text.size() - at);
                        REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data() + at), n), WaitForever)
                                        .error()
                                        .isOk());
                }
                CHECK(server.requests.load() - before <= 3); // two full buffers so far, not 1 429 writes
        }
        CHECK(server.nodes["/docs/a.txt"].data == text);
        CHECK(Vfs::stat("/srv/docs/a.txt", Root).value().size == 10000);
        const int before = server.requests.load();
        CHECK(readAll("/srv/docs/a.txt") == text);
        CHECK(server.requests.load() - before <= 8); // an open, three reads ahead, the end, a close

        // Seek to the end, append, truncate.
        {
                auto [h, err] = Vfs::open("/srv/docs/a.txt", Vfs::Write | Vfs::Append, Root);
                REQUIRE(err.isOk());
                REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>("!"), 1), WaitForever).error().isOk());
        }
        CHECK(server.nodes["/docs/a.txt"].data.size() == 10001);
        {
                auto [h, err] = Vfs::open("/srv/docs/a.txt", Vfs::Read | Vfs::Write, Root);
                REQUIRE(err.isOk());
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::End).value() == 10001);
                CHECK(h->stream().truncate(5) == Error::Ok);
        }
        CHECK(readAll("/srv/docs/a.txt") == "abcde");

        // Listing, in batches.
        for (const char *name : {"/srv/docs/b", "/srv/docs/c", "/srv/docs/d"}) {
                auto [h, err] = Vfs::open(StringView(name), Vfs::Write | Vfs::Create, Root);
                CHECK(err.isOk());
        }
        {
                auto [dir, err] = Vfs::openDirectory("/srv/docs", Root);
                REQUIRE(err.isOk());
                std::vector<std::string> names;
                for (;;) {
                        auto [info, ierr] = dir.next();
                        if (ierr.isError()) break;
                        names.emplace_back(info.name.data(), info.name.size());
                }
                CHECK(names == std::vector<std::string>{"a.txt", "b", "c", "d"});
        }
        CHECK(Vfs::openDirectory("/srv/docs/b", Root).error() == Error::NotDir);

        // Rename, time, remove, rmdir; statfs; errors as the server's.
        CHECK(Vfs::rename("/srv/docs/b", "/srv/docs/c", Vfs::RenameMode::NoReplace, Root) == Error::Exists);
        CHECK(Vfs::rename("/srv/docs/b", "/srv/docs/e", Vfs::RenameMode::Replace, Root) == Error::Ok);
        CHECK(Vfs::stat("/srv/docs/b", Root).error() == Error::NotExist);
        CHECK(Vfs::setModified("/srv/docs/e", bmds::Timestamp::fromSeconds(1000), Root) == Error::Ok);
        CHECK(Vfs::stat("/srv/docs/e", Root).value().modified.seconds() == 1000);
        CHECK(Vfs::rmdir("/srv/docs", Root) == Error::NotEmpty);
        for (const char *name : {"/srv/docs/a.txt", "/srv/docs/c", "/srv/docs/d", "/srv/docs/e"})
                CHECK(Vfs::remove(StringView(name), Root) == Error::Ok);
        CHECK(Vfs::rmdir("/srv/docs", Root) == Error::Ok);
        CHECK(Vfs::open("/srv/none", Vfs::Read, Root).error() == Error::NotExist);
        auto [fs, ferr] = Vfs::statfs("/srv", Root);
        CHECK(ferr.isOk());
        CHECK(fs.totalBytes == (1u << 20));
        CHECK(StringView(fs.type) == "testfs");

        // The server's end closed: the mount goes.
        server.stop();
        CHECK(!Vfs::isMounted("/srv"));
}

TEST_CASE("servedfs: many callers at once, answered out of order") {
        REQUIRE(Vfs::init().isOk());
        TestServer server;
        server.reversed = 4;
        server.nodes["/f"] = TestServer::Node{false, "x", 0};
        {
                auto [end, err] = Served::serve(configFor("/srv", 2000, 0)); // no attribute cache
                REQUIRE(err.isOk());
                server.start(std::move(end));
        }
        std::atomic<int>         ok{0};
        std::vector<std::thread> callers;
        for (int i = 0; i < 8; ++i)
                callers.emplace_back([&] {
                        for (int k = 0; k < 5; ++k)
                                if (Vfs::stat("/srv/f", Root).isOk()) ++ok;
                });
        for (std::thread &t : callers) t.join();
        CHECK(ok.load() == 40);
        server.stop();
}

TEST_CASE("servedfs: a request unanswered times out; the server gone fails what waits") {
        REQUIRE(Vfs::init().isOk());
        TestServer server;
        {
                auto [end, err] = Served::serve(configFor("/srv", 200, 0));
                REQUIRE(err.isOk());
                server.start(std::move(end));
        }
        server.ignore = true;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(Vfs::stat("/srv/f", Root).error() == Error::IoError);
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(190));

        // A call waiting when the server goes: it fails at once.
        std::atomic<bool> failed{false};
        std::thread       caller([&] { failed = Vfs::stat("/srv/g", Root).error() == Error::NoMedia; });
        while (server.requests.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        server.stop();
        caller.join();
        CHECK(failed.load());
        CHECK(!Vfs::isMounted("/srv"));
        CHECK(Vfs::stat("/srv/f", Root).error() == Error::NotExist);
}

TEST_CASE("servedfs: bad configurations and an unmount the server hears of") {
        REQUIRE(Vfs::init().isOk());
        Served::Config c = configFor("/srv");
        c.maxData = 1000;
        CHECK(Served::serve(c).error() == Error::Invalid);
        c = configFor("/dev/x");
        CHECK(Served::serve(c).error() == Error::Invalid);
        c = configFor("/srv");
        c.errorOf = nullptr;
        CHECK(Served::serve(c).error() == Error::Invalid);

        // Unmounted by someone else: the server's reads end.
        TestServer server;
        {
                auto [end, err] = Served::serve(configFor("/srv"));
                REQUIRE(err.isOk());
                server.start(std::move(end));
        }
        CHECK(Vfs::unmount("/srv") == Error::Ok);
        server.thread.join(); // its read got the end
        server.end.reset();
}
