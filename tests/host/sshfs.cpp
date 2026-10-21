/**
 * @file      sshfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * sshfs's core (programs/sshfs, devplan/netfs.md step 3) end to end, without the network: the
 * VFS's calls go to a served mount, whose requests sshfs's core makes into SFTP, which our
 * sftp-server's core answers from a directory of the host's (as the board would from a PC).
 */

#include <doctest.h>

#include <deque>
#include <fstream>
#include <ftw.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <bmds/process.h>
#include <bmds/servedfs.h>
#include <bmds/vfs.h>
#include "../../programs/sftp-server/sftp.h"
#include "../../programs/sshfs/sshfs.h"
#include "hostmemory.h"

extern "C" const sftp_fs_t *sshfs_test_posix_fs(void);

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

        // A directory of the host's for the test, on disk ($TMPDIR, else /tmp), removed after.
        class ScratchDir {
                public:
                        ScratchDir() {
                                const char *base = std::getenv("TMPDIR");
                                std::string t = std::string(base != nullptr ? base : "/tmp") + "/bmds-sshfs-XXXXXX";
                                std::vector<char> name(t.begin(), t.end());
                                name.push_back(0);
                                REQUIRE(mkdtemp(name.data()) != nullptr);
                                path = name.data();
                        }
                        ~ScratchDir() {
                                (void)nftw(
                                        path.c_str(),
                                        [](const char *p, const struct stat *, int, struct FTW *) {
                                                return ::remove(p);
                                        },
                                        16, FTW_DEPTH | FTW_PHYS);
                        }
                        std::string path;
        };

        // sshfs's two calls into our sftp-server's core, in this process: a request answered as
        // it is sent, the replies read back in order.
        class Loop {
                public:
                        Loop() {
                                sftp_init(&server, sshfs_test_posix_fs(), nullptr);
                                out.resize(SFTP_MAX_PACKET + 1024);
                        }
                        ~Loop() { sftp_finish(&server); }

                        static int send(void *ctx, const void *data, size_t size) {
                                auto       *l = static_cast<Loop *>(ctx);
                                const auto *p = static_cast<const uint8_t *>(data);
                                l->pending.insert(l->pending.end(), p, p + size);
                                while (l->pending.size() >= 4) {
                                        const uint32_t n = uint32_t(l->pending[0]) << 24 |
                                                           uint32_t(l->pending[1]) << 16 |
                                                           uint32_t(l->pending[2]) << 8 | l->pending[3];
                                        if (l->pending.size() < 4 + size_t(n)) break;
                                        ++l->requests;
                                        sftp_out_t o{l->out.data(), l->out.size(), 0};
                                        if (sftp_request(&l->server, l->pending.data() + 4, n, &o) != 0) return -1;
                                        l->replies.insert(l->replies.end(), o.data, o.data + o.length);
                                        l->pending.erase(l->pending.begin(), l->pending.begin() + 4 + n);
                                }
                                return 0;
                        }

                        static int recv(void *ctx, void *data, size_t size) {
                                auto *l = static_cast<Loop *>(ctx);
                                if (l->replies.size() < size) return -1;
                                auto *p = static_cast<uint8_t *>(data);
                                for (size_t i = 0; i < size; ++i) {
                                        p[i] = l->replies.front();
                                        l->replies.pop_front();
                                }
                                return 0;
                        }

                        sftp_server_t        server{};
                        std::vector<uint8_t> pending, out;
                        std::deque<uint8_t>  replies;
                        int                  requests = 0;
        };

        // sshfs's serving loop (main.c's), on a thread: until the mount goes.
        class Sshfs {
                public:
                        Sshfs(const std::string &dir, const char *mount) {
                                const sshfs_io_t io{Loop::send, Loop::recv, &loop};
                                char             why[160];
                                started = sshfs_start(&fs, &io, dir.c_str(), mount, why, sizeof(why)) == 0;
                                if (!started) return;
                                Served::Config c;
                                c.path = mount;
                                c.type = "sshfs";
                                c.maxData = fs.max_data;
                                c.attrMs = 0; // every stat asked: the host's files change under it here
                                c.errorOf = bmds::Process::errorFor;
                                auto [end, err] = Served::serve(c);
                                REQUIRE(err.isOk());
                                thread = std::thread([this, h = std::move(end)] { run(h); });
                        }
                        ~Sshfs() {
                                if (thread.joinable()) thread.join();
                                sshfs_finish(&fs);
                        }

                        void run(const FileHandle::Ptr &h) {
                                std::vector<uint8_t> request(Served::requestMax(fs.max_data)),
                                        reply(sshfs_reply_max(&fs));
                                for (;;) {
                                        auto [n, err] =
                                                h->read(WritableByteSpan(request.data(), request.size()), WaitForever);
                                        if (err.isError()) break; // unmounted
                                        const size_t m = sshfs_answer(&fs, request.data(), n, reply.data());
                                        REQUIRE(m != 0);
                                        (void)h->write(ByteSpan(reply.data(), m), WaitForever);
                                }
                                return;
                        }

                        Loop        loop;
                        sshfs_t     fs{};
                        bool        started = false;
                        std::thread thread;
        };

        std::string readVfs(const char *path) {
                auto [h, err] = Vfs::open(StringView(path), Vfs::Read, Root);
                REQUIRE(err.isOk());
                std::string out;
                char        buf[3000];
                for (;;) {
                        auto [n, rerr] =
                                h->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                        if (rerr == Error::EndOfFile) break;
                        REQUIRE(rerr.isOk());
                        out.append(buf, n);
                }
                return out;
        }

        void writeVfs(const char *path, const std::string &data,
                      uint8_t flags = Vfs::Write | Vfs::Create | Vfs::Truncate) {
                auto [h, err] = Vfs::open(StringView(path), flags, Root);
                REQUIRE(err.isOk());
                for (size_t at = 0; at < data.size(); at += 1000) {
                        const size_t n = std::min<size_t>(1000, data.size() - at);
                        REQUIRE(h->write(ByteSpan(reinterpret_cast<const uint8_t *>(data.data() + at), n), WaitForever)
                                        .error()
                                        .isOk());
                }
        }

        std::string readHost(const std::string &path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

} // namespace

TEST_CASE("sshfs: a directory served over SFTP") {
        REQUIRE(Vfs::init().isOk());
        ScratchDir dir;
        Sshfs      s(dir.path, "/net/pc");
        REQUIRE(s.started);
        CHECK(s.fs.max_data == 65536); // our server's limits@openssh.com
        CHECK(s.fs.posix_rename);
        CHECK(s.fs.statvfs);
        CHECK(StringView(s.fs.root) == StringView(dir.path.c_str()));

        // 200 000 bytes written 1 000 at a time: on the host whole; read back.
        std::string text;
        for (int i = 0; i < 200000; ++i) text.push_back(char('a' + i % 23));
        int before = s.loop.requests;
        writeVfs("/net/pc/a.txt", text);
        CHECK(s.loop.requests - before <= 10); // an open (two), four writes, a close
        CHECK(readHost(dir.path + "/a.txt") == text);
        CHECK(Vfs::stat("/net/pc/a.txt", Root).value().size == 200000);
        before = s.loop.requests;
        CHECK(readVfs("/net/pc/a.txt") == text);
        CHECK(s.loop.requests - before <= 10); // an open, four reads, the end, a close

        // Appended, truncated.
        writeVfs("/net/pc/a.txt", "!", Vfs::Write | Vfs::Append);
        CHECK(readHost(dir.path + "/a.txt").size() == 200001);
        {
                auto [h, err] = Vfs::open("/net/pc/a.txt", Vfs::Read | Vfs::Write, Root);
                REQUIRE(err.isOk());
                CHECK(h->stream().truncate(5) == Error::Ok);
        }
        CHECK(readHost(dir.path + "/a.txt") == "abcde");

        // A directory of 150 (more than one listing's batch), listed whole.
        CHECK(Vfs::mkdir("/net/pc/many", Root) == Error::Ok);
        CHECK(Vfs::mkdir("/net/pc/many", Root) == Error::Exists);
        for (int i = 0; i < 150; ++i) std::ofstream(dir.path + "/many/file-with-a-long-name-" + std::to_string(i));
        {
                auto [d, err] = Vfs::openDirectory("/net/pc/many", Root);
                REQUIRE(err.isOk());
                int count = 0;
                for (;;) {
                        auto [info, ierr] = d.next();
                        if (ierr.isError()) break;
                        CHECK(info.name.view().startsWith("file-with-a-long-name-"));
                        ++count;
                }
                CHECK(count == 150);
        }
        CHECK(Vfs::openDirectory("/net/pc/a.txt", Root).error() == Error::NotDir);

        // What SFTP's FAILURE was, as the VFS needs it.
        CHECK(Vfs::rmdir("/net/pc/many", Root) == Error::NotEmpty);
        CHECK(Vfs::rmdir("/net/pc/a.txt", Root) == Error::NotDir);
        CHECK(Vfs::remove("/net/pc/many", Root) == Error::IsDir);
        CHECK(Vfs::open("/net/pc/many", Vfs::Read, Root).error() == Error::IsDir);
        CHECK(Vfs::open("/net/pc/none", Vfs::Read, Root).error() == Error::NotExist);
        CHECK(Vfs::open("/net/pc/a.txt", Vfs::Write | Vfs::Create | Vfs::Exclusive, Root).error() == Error::Exists);
        writeVfs("/net/pc/b.txt", "bee");
        CHECK(Vfs::rename("/net/pc/b.txt", "/net/pc/a.txt", Vfs::RenameMode::NoReplace, Root) == Error::Exists);
        CHECK(Vfs::rename("/net/pc/many", "/net/pc/a.txt", Vfs::RenameMode::Replace, Root) == Error::NotDir);
        CHECK(Vfs::rename("/net/pc/b.txt", "/net/pc/a.txt", Vfs::RenameMode::Replace, Root) == Error::Ok);
        CHECK(readHost(dir.path + "/a.txt") == "bee");

        // Times.
        CHECK(Vfs::setModified("/net/pc/a.txt", bmds::Timestamp::fromSeconds(1700000000), Root) == Error::Ok);
        struct stat st;
        CHECK(::stat((dir.path + "/a.txt").c_str(), &st) == 0);
        CHECK(st.st_mtime == 1700000000);
        CHECK(Vfs::stat("/net/pc/a.txt", Root).value().modified.seconds() == 1700000000);

        // Links: the server's own; a target into the directory mounted shown at the mount.
        CHECK(Vfs::symlink("a.txt", "/net/pc/rel", Root) == Error::Ok);
        char target[1100] = {};
        CHECK(::readlink((dir.path + "/rel").c_str(), target, sizeof(target) - 1) == 5);
        CHECK(std::string(target) == "a.txt");
        CHECK(readVfs("/net/pc/rel") == "bee");
        CHECK(Vfs::lstat("/net/pc/rel", Root).value().type == FileType::Link);
        CHECK(Vfs::symlink("/net/pc/a.txt", "/net/pc/abs", Root) == Error::Ok);
        const ssize_t k = ::readlink((dir.path + "/abs").c_str(), target, sizeof(target) - 1);
        REQUIRE(k > 0);
        CHECK(std::string(target, size_t(k)) == dir.path + "/a.txt"); // the host's path there
        CHECK(Vfs::readLink("/net/pc/abs", Root).value().view() == "/net/pc/a.txt");
        CHECK(readVfs("/net/pc/abs") == "bee");
        CHECK(::symlink("/elsewhere/x", (dir.path + "/out").c_str()) == 0);
        CHECK(Vfs::readLink("/net/pc/out", Root).value().view() == "/elsewhere/x"); // as it is
        CHECK(Vfs::readLink("/net/pc/a.txt", Root).error() == Error::Invalid);
        CHECK(Vfs::symlink("x", "/net/pc/rel", Root) == Error::Exists);

        // Space.
        auto [info, ferr] = Vfs::statfs("/net/pc", Root);
        CHECK(ferr.isOk());
        CHECK(info.totalBytes > 0);
        CHECK(StringView(info.type) == "sshfs");

        // Unmounted: the serving loop ends.
        CHECK(Vfs::unmount("/net/pc") == Error::Ok);
        s.thread.join();
}

TEST_CASE("sshfs: a directory that isn't there, or isn't one") {
        ScratchDir dir;
        {
                Sshfs s(dir.path + "/none", "/net/pc");
                CHECK(!s.started);
        }
        std::ofstream(dir.path + "/file");
        Sshfs s(dir.path + "/file", "/net/pc");
        CHECK(!s.started);
}
