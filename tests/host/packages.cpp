/**
 * @file      packages.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Installed packages and the `pkg` command (devplan/packages.md §7, §9) on a tmpfs: packages
 * built here (signed with a test key), and the one tools/mkpkg made (tests/host/pkg, the dev
 * key). Installing, upgrading, removing, the refusals, power cuts, verify, icons, the JSON, and
 * dependencies: found in the files' folder, automatic, removed when nothing needs them; and the
 * sources: lists updated from a folder (its index, or one made here), names, upgrades, search.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <bmds/blake2s.h>
#include <bmds/ed25519.h>
#include <bmds/pkgcommand.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"

using bmds::CommandContext;
using bmds::Error;
using bmds::Shell;
using bmds::Span;
using bmds::StringView;
namespace Vfs = bmds::Vfs;
namespace Packages = bmds::Packages;

namespace {

        const Vfs::Context Root{"/", nullptr};

        constexpr uint8_t Seed[32] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6,
                                      7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2};

        StringView sv(const std::string &s) {
                return StringView(s.data(), s.size());
        }

        // The keys: the test key (trusted), then keys/dev.pub (the dev key).
        bmds::SignedContainer::Key keys[2];

        bool isDevKey(size_t index) {
                return index == 1;
        }

        const char *keyComment(size_t index) {
                return index == 0 ? "the test key" : "the dev key's comment";
        }

        // What a test changes of the configuration; the fixture puts it back.
        std::string         sources = "file:/t/repo";
        Packages::DevPolicy devPolicy = Packages::DevPolicy::Allow;
        constexpr uint8_t   OtherSeed[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                             17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

        // A web the fake fetcher serves: http://web/PATH is /t/web/PATH.
        struct Web {
                        bool                  on = true; ///< Config::fetch set at all.
                        int                   calls = 0;
                        std::vector<uint64_t> froms;         ///< Each fetch's first byte.
                        long                  cutAfter = -1; ///< The next fetch ends after this many bytes.
                        bool                  ignoreRange = false;
        } web;

        std::string get(const std::string &path);

        Error fakeFetch(StringView url, uint64_t from, const bmds::FetchSink &sink, bmds::String *finalUrl,
                        bmds::String &why) {
                // Its messages name what they are about, as `Http::get()`'s do.
                ++web.calls;
                web.froms.push_back(from);
                if (finalUrl != nullptr) (void)finalUrl->append(url);
                constexpr StringView Prefix("http://web/");
                if (!url.startsWith(Prefix)) {
                        (void)why.appendf("%.*s: no such host", int(url.size()), url.data());
                        return Error::NotExist;
                }
                const std::string content =
                        get("/t/web/" + std::string(url.mid(Prefix.size()).data(), url.size() - Prefix.size()));
                if (content == "<missing>") {
                        (void)why.appendf("%.*s: the server answered 404 (not found)", int(url.size()), url.data());
                        return Error::NotExist;
                }
                const uint64_t start = web.ignoreRange ? 0 : from;
                Error          err = sink.begin(sink.arg, start, content.size());
                std::string    data = content.substr(start);
                const bool     cut = web.cutAfter >= 0 && size_t(web.cutAfter) < data.size();
                if (cut) data = data.substr(0, size_t(web.cutAfter));
                web.cutAfter = -1;
                if (err.isOk() && !data.empty())
                        err = sink.body(sink.arg,
                                        bmds::ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
                if (err.isOk() && cut) {
                        (void)why.appendf("%.*s: the connection ended early", int(url.size()), url.data());
                        return Error::CorruptData;
                }
                return err;
        }

        Packages::Config config() {
                static bool ready = false;
                if (!ready) {
                        uint8_t unused[64];
                        bmds::Ed25519::sign(Seed, "", 0, unused, keys[0].bytes);
                        std::ifstream pub(BMDS_TEST_KEYS_DIR "/dev.pub");
                        std::string   kind, hex;
                        pub >> kind >> hex;
                        REQUIRE(hex.size() == 64);
                        for (size_t i = 0; i < 32; ++i)
                                keys[1].bytes[i] = uint8_t(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
                        ready = true;
                }
                Packages::Config c;
                c.root = "/t/root";
                c.db = "/t/root/var/lib/pkg";
                c.keys = Span<const bmds::SignedContainer::Key>(keys, 2);
                c.isDevKey = isDevKey;
                c.keyComment = keyComment;
                c.devKey = &keys[1];
                c.keysDir = "/t/keys";
                c.dev = devPolicy;
                c.arch = "esp32p4";
                c.abi = 1;
                c.cache = "/t/root/var/cache/pkg";
                c.sources = sv(sources);
                c.fetch = web.on ? fakeFetch : nullptr;
                return c;
        }

        void put(const std::string &path, const std::string &bytes) {
                auto [file, err] = Vfs::open(sv(path), Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                auto [n, werr] =
                        file->write(bmds::ByteSpan(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()),
                                    bmds::WaitForever);
                REQUIRE(werr.isOk());
                REQUIRE(n == bytes.size());
        }

        // The file's bytes, or "<missing>".
        std::string get(const std::string &path) {
                auto [file, err] = Vfs::open(sv(path), Vfs::Read, Root);
                if (err.isError()) return "<missing>";
                std::string out;
                char        buf[4096];
                for (;;) {
                        auto [n, rerr] =
                                file->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                           bmds::WaitForever);
                        if (rerr.isError() || n == 0) return out;
                        out.append(buf, n);
                }
        }

        bool exists(const std::string &path) {
                return Vfs::lstat(sv(path), Root).isOk();
        }

        std::string link(const std::string &path) {
                auto [target, err] = Vfs::readLink(sv(path), Root);
                return err.isOk() ? std::string(target.data(), target.size()) : "<not a link>";
        }

        void le16(std::string &s, uint16_t v) {
                s += char(v & 0xff);
                s += char(v >> 8);
        }

        void le32(std::string &s, uint32_t v) {
                for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff);
        }

        void le64(std::string &s, uint64_t v) {
                for (int i = 0; i < 8; ++i) s += char((v >> (8 * i)) & 0xff);
        }

        std::string padded(const std::string &text, size_t n) {
                std::string s = text;
                s.resize(n, '\0');
                return s;
        }

        std::string blake2s(const std::string &data) {
                bmds::Blake2s h;
                h.update(data.data(), data.size());
                uint8_t d[32];
                h.final(d);
                return std::string(reinterpret_cast<const char *>(d), 32);
        }

        std::string hexOf(const std::string &bytes) {
                static const char Digits[] = "0123456789abcdef";
                std::string       out;
                for (unsigned char c : bytes) {
                        out += Digits[c >> 4];
                        out += Digits[c & 15];
                }
                return out;
        }

        // What a test package holds.
        struct Spec {
                        std::string                                      name;
                        std::string                                      version;
                        std::string                                      fields; // manifest field lines
                        std::vector<std::string>                         dirs;
                        std::vector<std::pair<std::string, std::string>> files; // path, bytes
                        std::vector<std::pair<std::string, std::string>> links; // path, target
                        std::string                                      arch = "esp32p4";
                        uint32_t                                         abi = 1;
        };

        // A package of @p spec, signed with @p seed (the trusted test key's), its blocks stored.
        std::string build(const Spec &spec, const uint8_t (&seed)[32] = Seed) {
                std::string manifest = spec.fields, data;
                uint64_t    installed = 0;
                for (const auto &d : spec.dirs) manifest += "d\t" + d + "\n";
                for (const auto &f : spec.files) {
                        manifest += "f\t" + f.first + "\t644\t" + std::to_string(f.second.size()) + "\t" +
                                    hexOf(blake2s(f.second)) + "\n";
                        for (size_t at = 0; at < f.second.size(); at += 65536) {
                                const std::string block = f.second.substr(at, 65536);
                                le32(data, uint32_t(block.size()) | 0x80000000u);
                                data += block;
                        }
                        installed += f.second.size();
                }
                for (const auto &l : spec.links) manifest += "l\t" + l.first + "\t" + l.second + "\n";

                uint8_t                    id[8];
                bmds::SignedContainer::Key signer;
                uint8_t                    unusedSignature[64];
                bmds::Ed25519::sign(seed, "", 0, unusedSignature, signer.bytes);
                bmds::SignedContainer::keyId(signer, id);
                const std::vector<std::pair<uint32_t, std::string>> sections = {{1, manifest}, {2, data}};
                std::string                                         header = "BMPK";
                le16(header, 1);
                le16(header, 128);
                le32(header, 0);
                le32(header, uint32_t(sections.size()));
                header.append(reinterpret_cast<const char *>(id), 8);
                header += padded(spec.name, 32) + padded(spec.version, 32) + padded(spec.arch, 16);
                le32(header, spec.abi);
                le32(header, 0);
                le64(header, installed);
                le64(header, 1700000000);
                REQUIRE(header.size() == 128);
                size_t offset = 128 + sections.size() * 48 + 64;
                for (const auto &s : sections) {
                        le32(header, s.first);
                        le32(header, 0);
                        le32(header, uint32_t(offset));
                        le32(header, uint32_t(s.second.size()));
                        header += blake2s(s.second);
                        offset += s.second.size();
                }
                uint8_t signature[64], unused[32];
                bmds::Ed25519::sign(seed, header.data(), header.size(), signature, unused);
                std::string out = header + std::string(reinterpret_cast<const char *>(signature), 64);
                for (const auto &s : sections) out += s.second;
                return out;
        }

        // Writes @p spec's package to DIR/NAME_VERSION.bmpk and returns the path.
        std::string make(const Spec &spec, const std::string &dir = "/t") {
                const std::string path = dir + "/" + spec.name + "_" + spec.version + ".bmpk";
                put(path, build(spec));
                return path;
        }

        std::string fixture(const std::string &name) {
                std::ifstream file(std::string(BMDS_TEST_PKG_DIR "/") + name, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        int runPkg(CommandContext &ctx) {
                return bmds::PkgCommand::run(ctx, config());
        }

        constexpr bmds::Command Commands[] = {
                {"pkg", bmds::PkgCommand::Usage, bmds::PkgCommand::Help, runPkg},
        };

        // A tmpfs at /t and a shell with `pkg`.
        struct Fixture {
                        Fixture() {
                                sources = "file:/t/repo";
                                devPolicy = Packages::DevPolicy::Allow;
                                web = Web{};
                                REQUIRE(Vfs::init().isOk());
                                REQUIRE(Vfs::mountTmpfs("/t", 8 * 1024 * 1024).isOk());
                                REQUIRE(Vfs::mkdir("/t/root", Root).isOk());
                                shell = std::make_unique<Shell>(con, Span<const bmds::Command>(Commands), 1 << 20);
                                REQUIRE(shell->init().isOk());
                                (void)config();
                        }

                        ~Fixture() {
                                shell.reset();
                                (void)Vfs::unmount("/t");
                        }

                        // Runs @p line; returns stdout and stderr together.
                        std::string run(const std::string &line, int expect = Shell::ExitSuccess) {
                                const int   status = shell->execute(line.c_str());
                                std::string out = con.take();
                                CHECK_MESSAGE(status == expect, line, " -> ", out);
                                return out;
                        }

                        FakeConsole            con;
                        std::unique_ptr<Shell> shell;
        };

        bool contains(const std::string &text, const std::string &part) {
                return text.find(part) != std::string::npos;
        }

        // The line of @p text that starts with @p name and a space, its runs of spaces made one.
        std::string lineOf(const std::string &text, const std::string &name) {
                size_t at = 0;
                while (at < text.size()) {
                        size_t            end = text.find('\n', at);
                        const std::string line =
                                text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                        if (line.rfind(name + " ", 0) == 0) {
                                std::string out;
                                for (char c : line) {
                                        if (c != ' ' || out.empty() || out.back() != ' ') out += c;
                                }
                                return out;
                        }
                        if (end == std::string::npos) break;
                        at = end + 1;
                }
                return "<no line for " + name + ">";
        }

        const Spec Busybox = {"busybox",
                              "1.37.0-1",
                              "title\tBusyBox\nsummary\tUnix tools\n",
                              {"bin"},
                              {{"bin/busybox", "busybox binary v1"}},
                              {{"bin/awk", "busybox"}, {"bin/sed", "busybox"}}};
        const Spec Tzdata = {"tzdata", "2025b-1", "", {"share", "share/zoneinfo"}, {{"share/zoneinfo/UTC", "TZif"}},
                             {}};

} // namespace

TEST_CASE("Packages: installed, listed, shown, verified, removed") {
        Fixture f;
        f.run("pkg install -y " + make(Busybox));
        CHECK(get("/t/root/bin/busybox") == "busybox binary v1");
        CHECK(link("/t/root/bin/awk") == "busybox");
        CHECK(link("/t/root/bin/sed") == "busybox");
        CHECK(exists("/t/root/var/lib/pkg/busybox.status"));
        CHECK(exists("/t/root/var/lib/pkg/busybox.manifest"));
        CHECK_FALSE(exists("/t/root/var/lib/pkg/busybox.pending"));
        CHECK_FALSE(exists("/t/root/bin/busybox.pkgnew"));

        const std::string list = f.run("pkg list");
        CHECK(contains(list, "busybox"));
        CHECK(contains(list, "1.37.0-1"));
        CHECK(contains(list, "Unix tools"));
        const std::string info = f.run("pkg info busybox --files");
        CHECK(contains(info, "Title:    BusyBox"));
        CHECK(contains(info, "Signed:   the test key (a built-in trusted key; key "));
        CHECK(contains(info, "bin/awk -> busybox"));
        CHECK(contains(f.run("pkg verify"), "every file matches"));

        // A changed file and a missing link are found.
        put("/t/root/bin/busybox", "tampered");
        REQUIRE(Vfs::remove("/t/root/bin/sed", Root).isOk());
        const std::string bad = f.run("pkg verify busybox", Shell::ExitFailure);
        CHECK(contains(bad, "bin/busybox: changed"));
        CHECK(contains(bad, "bin/sed: missing"));

        f.run("pkg remove -y busybox");
        CHECK_FALSE(exists("/t/root/bin/busybox"));
        CHECK_FALSE(exists("/t/root/bin/awk"));
        CHECK_FALSE(exists("/t/root/bin")); // emptied, so removed
        CHECK_FALSE(exists("/t/root/var/lib/pkg/busybox.status"));
        CHECK(f.run("pkg list").empty());
        CHECK(contains(f.run("pkg remove -y busybox", Shell::ExitFailure), "busybox isn't installed"));
}

TEST_CASE("Packages: dependencies, needed before and kept after") {
        Fixture           f;
        const std::string hello = "/t/hello_2.0~rc1-3.bmpk";
        put(hello, fixture("hello_2.0~rc1-3.bmpk"));
        CHECK(contains(f.run("pkg install -y " + hello, Shell::ExitFailure),
                       "busybox (>= 1.37.0-1), needed by hello, isn't installed or given (none in /t)"));
        CHECK_FALSE(exists("/t/root/bin/hello"));
        f.run("pkg install -y " + make(Busybox) + " " + make(Tzdata));
        const std::string out = f.run("pkg install -y " + hello);
        CHECK(contains(out, "hello 1:2.0~rc1-3: installed"));
        CHECK(get("/t/root/bin/hello") == "#!/bin/sh\necho hello\n");
        CHECK(get("/t/root/share/hello/big.bin").size() == 85536); // a stored and a compressed block
        CHECK(get("/t/root/share/hello/empty").empty());
        CHECK(link("/t/root/bin/hi") == "hello");
        CHECK(contains(f.run("pkg info hello"), "Signed:   the dev key (anyone could have; key 1edc0f2cc301cc37)"));

        // What hello needs stays while hello is there; both can go together.
        CHECK(contains(f.run("pkg remove -y busybox", Shell::ExitFailure), "hello needs busybox: remove it too"));
        // A dry run and the plan check it too.
        CHECK(contains(f.run("pkg remove --dry-run busybox", Shell::ExitFailure), "hello needs busybox"));
        CHECK(contains(f.run("pkg --json remove --dry-run busybox", Shell::ExitFailure), "\"type\":\"error\""));
        CHECK(contains(f.run("pkg remove --dry-run busybox hello"), "remove busybox"));
        CHECK(exists("/t/root/bin/hello"));
        f.run("pkg remove -y busybox hello");
        CHECK_FALSE(exists("/t/root/bin/hello"));
        CHECK(exists("/t/root/share/zoneinfo/UTC")); // tzdata's
        CHECK_FALSE(exists("/t/root/share/hello"));

        // The icon, as installed.
        f.run("pkg install -y " + make(Busybox));
        f.run("pkg install -y " + hello);
        f.run("pkg icon hello --size 20 -o /t/icon.png");
        const std::string png = get("/t/icon.png");
        REQUIRE(png.size() > 24);
        CHECK(png.substr(1, 3) == "PNG");
        CHECK(uint8_t(png[19]) == 32); // the next bigger than 20
        f.run("pkg icon hello --size 16 -o /t/icon.png");
        CHECK(uint8_t(get("/t/icon.png")[19]) == 16);
        CHECK(contains(f.run("pkg icon busybox -o /t/x.png", Shell::ExitFailure), "no icon"));
}

TEST_CASE("Packages: an upgrade replaces, adds and drops; a downgrade is said") {
        Fixture f;
        Spec    v1 = {"tool",
                      "1.0-1",
                      "",
                      {"bin", "share", "share/tool"},
                      {{"bin/tool", "v1"}, {"share/tool/old.txt", "old"}},
                      {{"bin/t", "tool"}}};
        Spec    v2 = {"tool",
                      "2.0-1",
                      "",
                      {"bin", "share", "share/tool"},
                      {{"bin/tool", "v2"}, {"share/tool/new.txt", "new"}},
                      {{"bin/t", "tool"}}};
        f.run("pkg install -y " + make(v1));
        CHECK(contains(f.run("pkg install --dry-run " + make(v2)), "upgrade tool 2.0-1 (from 1.0-1)"));
        CHECK(get("/t/root/bin/tool") == "v1"); // a dry run writes nothing
        f.run("pkg install -y " + make(v2));
        CHECK(get("/t/root/bin/tool") == "v2");
        CHECK(get("/t/root/share/tool/new.txt") == "new");
        CHECK_FALSE(exists("/t/root/share/tool/old.txt"));
        CHECK(link("/t/root/bin/t") == "tool");
        CHECK(contains(f.run("pkg install -y " + make(v1)), "tool: going back from 2.0-1 to 1.0-1"));
        CHECK(get("/t/root/bin/tool") == "v1");
        CHECK_FALSE(exists("/t/root/share/tool/new.txt"));
}

TEST_CASE("Packages: files another package owns, and unowned ones in the way") {
        Fixture f;
        f.run("pkg install -y " + make(Busybox));
        Spec thief = {"thief", "1", "", {"bin"}, {{"bin/busybox", "mine now"}}, {}};
        CHECK(contains(f.run("pkg install -y " + make(thief), Shell::ExitFailure), "bin/busybox belongs to busybox"));
        CHECK(get("/t/root/bin/busybox") == "busybox binary v1");

        // An unowned file with the same bytes is taken over; a different one only with --overwrite.
        Spec tool = {"tool", "1", "", {"bin"}, {{"bin/tool", "tool bytes"}}, {{"bin/t", "tool"}}};
        put("/t/root/bin/tool", "tool bytes");
        REQUIRE(Vfs::symlink("tool", "/t/root/bin/t", Root).isOk());
        f.run("pkg install -y " + make(tool));
        f.run("pkg remove -y tool");
        put("/t/root/bin/tool", "someone else's");
        const std::string path = make(tool);
        CHECK(contains(f.run("pkg install -y " + path, Shell::ExitFailure),
                       "/t/root/bin/tool is in the way (no package's, and different): --overwrite replaces it"));
        CHECK(get("/t/root/bin/tool") == "someone else's");
        f.run("pkg install -y --overwrite " + path);
        CHECK(get("/t/root/bin/tool") == "tool bytes");
}

TEST_CASE("Packages: what is refused leaves nothing behind") {
        Fixture f;
        Spec    p = {"thing", "1", "", {"share"}, {{"share/thing", std::string(100000, 'x')}}, {}};

        std::string damaged = build(p);
        damaged[damaged.size() - 10] ^= 1;
        put("/t/damaged.bmpk", damaged);
        CHECK(contains(f.run("pkg install -y /t/damaged.bmpk", Shell::ExitFailure), "damaged"));

        Spec other = p;
        other.arch = "esp32s3";
        CHECK(contains(f.run("pkg install -y " + make(other), Shell::ExitFailure), "for esp32s3, not esp32p4"));
        Spec newer = p;
        newer.abi = 2;
        CHECK(contains(f.run("pkg install -y " + make(newer), Shell::ExitFailure),
                       "needs a newer BMDS (ABI 2; this is 1)"));

        std::string unsigned_ = build(p);
        unsigned_[200] ^= 1; // the signature
        put("/t/unsigned.bmpk", unsigned_);
        CHECK(contains(f.run("pkg install -y /t/unsigned.bmpk", Shell::ExitFailure), "not signed by a key"));

        put("/t/not.bmpk", "not a package at all");
        CHECK(contains(f.run("pkg install -y /t/not.bmpk", Shell::ExitFailure), "not a package"));
        CHECK(contains(f.run("pkg install -y /t/missing.bmpk", Shell::ExitFailure), "/t/missing.bmpk"));
        CHECK(contains(f.run("pkg install -y busybox", Shell::ExitFailure),
                       "busybox: no such package in the lists (pkg update reads them)"));

        CHECK_FALSE(exists("/t/root/share"));
        CHECK_FALSE(exists("/t/root/var/lib/pkg/thing.pending"));
        CHECK(f.run("pkg list").empty());
}

TEST_CASE("Packages: a power cut, finished by the next pkg") {
        Fixture           f;
        const std::string path = make(Busybox);
        // An installation cut short: the pending record, a file half in place.
        REQUIRE(Vfs::mkdir("/t/root/var", Root).isOk());
        REQUIRE(Vfs::mkdir("/t/root/var/lib", Root).isOk());
        REQUIRE(Vfs::mkdir("/t/root/var/lib/pkg", Root).isOk());
        REQUIRE(Vfs::mkdir("/t/root/bin", Root).isOk());
        put("/t/root/var/lib/pkg/busybox.pending", "action\tinstall\nfile\t" + path + "\n");
        put("/t/root/bin/busybox", "busybox binary v1"); // renamed into place before the cut
        put("/t/root/bin/sed.pkgnew", "junk");
        const std::string out = f.run("pkg install -y " + make(Tzdata));
        CHECK(contains(out, "busybox: installing it again: its installation was cut short"));
        CHECK(contains(f.run("pkg list"), "busybox"));
        CHECK(link("/t/root/bin/sed") == "busybox");
        CHECK_FALSE(exists("/t/root/var/lib/pkg/busybox.pending"));

        // A removal cut short is finished.
        put("/t/root/var/lib/pkg/tzdata.pending", "action\tremove\n");
        CHECK(contains(f.run("pkg list"), "tzdata")); // list doesn't recover
        CHECK(contains(f.run("pkg remove -y busybox"), "tzdata: finishing its removal"));
        CHECK_FALSE(exists("/t/root/share/zoneinfo/UTC"));
        CHECK(f.run("pkg list").empty());

        // A cut-short installation whose file is gone is said, and given up.
        put("/t/root/var/lib/pkg/gone.pending", "action\tinstall\nfile\t/t/gone.bmpk\n");
        CHECK(contains(f.run("pkg install -y " + make(Tzdata), Shell::ExitFailure),
                       "gone: its installation was cut short, and /t/gone.bmpk is gone"));
        f.run("pkg install -y " + make(Tzdata));
}

TEST_CASE("Packages: --json, one record a line") {
        Fixture           f;
        const std::string out = f.run("pkg --json install -y " + make(Busybox));
        CHECK(contains(out, "{\"type\":\"progress\",\"package\":\"busybox\",\"step\":\"writing\""));
        CHECK(contains(out,
                       "{\"type\":\"done\",\"action\":\"installed\",\"name\":\"busybox\",\"version\":\"1.37.0-1\"}\n"));
        const std::string list = f.run("pkg --json list");
        CHECK(list.rfind("{\"type\":\"package\",\"name\":\"busybox\",\"version\":\"1.37.0-1\"", 0) == 0);
        CHECK(contains(list, "\"title\":\"BusyBox\""));
        CHECK(contains(list, "\"signer\":\"trusted\""));
        CHECK(contains(list, "\"automatic\":false"));
        CHECK(std::count(list.begin(), list.end(), '\n') == 1);
        CHECK(contains(f.run("pkg --json install " + make(Tzdata), Shell::ExitFailure),
                       "{\"type\":\"error\",\"package\":\"\",\"error\":\"Invalid\",\"text\":\"--json doesn't ask: give "
                       "-y or --dry-run\"}"));
        const std::string plan = f.run("pkg --json install --dry-run " + make(Busybox));
        CHECK(contains(plan, "{\"type\":\"plan\",\"action\":\"reinstall\",\"name\":\"busybox\""));
        CHECK(contains(f.run("pkg --json info busybox --files"),
                       "{\"type\":\"file\",\"package\":\"busybox\",\"path\":"
                       "\"bin/awk\",\"kind\":\"link\",\"target\":\"busybox\"}"));
        CHECK(contains(f.run("pkg --json remove -y tzdata", Shell::ExitFailure), "\"error\":\"NotFound\""));
}

namespace {

        // A folder of packages, as a card's: a meta package, an application, two versions of what
        // it needs (one too old), and busybox.
        struct Repo {
                        std::string set, app, lib1, lib2, busybox;

                        Repo() {
                                REQUIRE(Vfs::mkdir("/t/repo", Root).isOk());
                                set = make({"set", "1", "title\tThe set\ndepends\tapp, busybox\n", {}, {}, {}, "all"},
                                           "/t/repo");
                                app = make({"app", "1", "depends\tlib (>= 2)\n", {"bin"}, {{"bin/app", "app"}}, {}},
                                           "/t/repo");
                                lib1 = make({"lib", "1", "", {"share"}, {{"share/lib", "lib 1"}}, {}}, "/t/repo");
                                lib2 = make({"lib", "2", "", {"share"}, {{"share/lib", "lib 2"}}, {}}, "/t/repo");
                                busybox = make(Busybox, "/t/repo");
                        }
        };

} // namespace

TEST_CASE("Packages: a meta package, its set found in its folder, automatic") {
        Fixture f;
        Repo    r;
        // The plan: each after what it needs, the dependencies marked; a dry run writes nothing.
        const std::string plan = f.run("pkg install --dry-run " + r.set);
        CHECK(plan.find("install lib 2") < plan.find("install app 1"));
        CHECK(plan.find("install app 1") < plan.find("install set 1"));
        CHECK(contains(plan, "install lib 2, 5 bytes, signed by the test key (a built-in trusted key) (a dependency)"));
        CHECK(contains(plan, "set 1: would install\n"));
        CHECK_FALSE(exists("/t/root/bin/app"));
        CHECK(f.run("pkg list").empty());

        const std::string out = f.run("pkg install -y " + r.set);
        CHECK(contains(out, "lib 2: installed (a dependency)"));
        CHECK(contains(out, "busybox 1.37.0-1: installed (a dependency)"));
        CHECK(contains(out, "set 1: installed\n"));
        CHECK(get("/t/root/share/lib") == "lib 2"); // the newest that suits
        const std::string list = f.run("pkg list");
        CHECK(contains(list, "auto        Unix tools"));
        CHECK(contains(list, "installed   The set"));
        CHECK(contains(f.run("pkg info app"), "installed as a dependency"));
        CHECK(contains(f.run("pkg autoremove"), "nothing to remove"));

        // Kept by being asked for: busybox stays when the set goes.
        CHECK(contains(f.run("pkg mark manual busybox"), "busybox 1.37.0-1: marked manual"));
        const std::string dry = f.run("pkg remove --dry-run set");
        CHECK(contains(dry, "app: would be needed no more (pkg autoremove removes it)"));
        CHECK_FALSE(contains(dry, "busybox:"));
        const std::string removed = f.run("pkg remove -y set");
        CHECK(contains(removed, "app: no longer needed (pkg autoremove removes it)"));
        CHECK(contains(removed, "lib: no longer needed"));
        const std::string plan2 = f.run("pkg autoremove --dry-run");
        CHECK(contains(plan2, "remove app 1 (needed no more)"));
        CHECK(contains(plan2, "remove lib 2 (needed no more)"));
        CHECK(exists("/t/root/bin/app"));
        f.run("pkg autoremove -y");
        CHECK_FALSE(exists("/t/root/bin/app"));
        CHECK_FALSE(exists("/t/root/share/lib"));
        CHECK(contains(f.run("pkg list"), "busybox"));
        CHECK(contains(f.run("pkg mark auto busybox"), "marked automatic"));
        f.run("pkg autoremove -y");
        CHECK(f.run("pkg list").empty());
        CHECK(contains(f.run("pkg mark auto busybox", Shell::ExitFailure), "busybox isn't installed"));
}

TEST_CASE("Packages: dependencies that can't be met, and plans that would break one") {
        Fixture f;
        Repo    r;
        // Nothing that suits: what was skipped is said.
        REQUIRE(Vfs::remove(sv(r.lib2), Root).isOk());
        make({"lib", "3", "", {"share"}, {{"share/lib", "lib 3"}}, {}, "esp32p4", 2}, "/t/repo");
        const std::string skipped = f.run("pkg install -y " + r.app, Shell::ExitFailure);
        CHECK(contains(skipped,
                       "lib (>= 2), needed by app, isn't installed or given (lib_3.bmpk: lib needs a newer BMDS (ABI "
                       "2; this is 1): update the firmware first)"));
        CHECK(f.run("pkg list").empty());

        // An installed package's dependency isn't taken below what it needs.
        make({"lib", "2", "", {"share"}, {{"share/lib", "lib 2"}}, {}}, "/t/repo");
        f.run("pkg install -y " + r.app);
        CHECK(contains(f.run("pkg install -y " + r.lib1, Shell::ExitFailure),
                       "installing lib 1 would break app, which needs lib (>= 2)"));
        CHECK(get("/t/root/share/lib") == "lib 2");
        // Given together, a version too old for what needs it.
        CHECK(contains(f.run("pkg install -y " + r.app + " " + r.lib1, Shell::ExitFailure),
                       "app needs lib (>= 2); 1 is given"));
        CHECK(contains(f.run("pkg install -y " + r.lib1 + " /t/repo/lib_2.bmpk", Shell::ExitFailure),
                       "lib is given twice (1 and 2)"));
        // Asked for after coming as a dependency: asked for now.
        f.run("pkg install -y /t/repo/lib_2.bmpk");
        CHECK_FALSE(contains(f.run("pkg list"), "  auto  "));

        // JSON: the plan says what comes as a dependency, and from which file.
        f.run("pkg remove -y app lib");
        const std::string json = f.run("pkg --json install --dry-run " + r.app);
        CHECK(contains(json, "\"name\":\"lib\",\"version\":\"2\",\"installedSize\":5,\"signer\":\"trusted\","
                             "\"automatic\":true,\"file\":\"/t/repo/lib_2.bmpk\"}"));
        CHECK(contains(json, "\"automatic\":false,\"file\":\"/t/repo/app_1.bmpk\"}"));
}

TEST_CASE("Packages: sources, their lists, and installing by name") {
        Fixture f;
        Repo    r;
        // No index in the folder: made here. The lists say what is available.
        CHECK(contains(f.run("pkg update"),
                       "file:/t/repo: 5 packages (indexed here: its index.bmpi is missing or out of date)"));
        const std::string list = f.run("pkg list");
        CHECK(lineOf(list, "lib") == "lib 2 available ");
        CHECK(lineOf(list, "set") == "set 1 available The set");
        CHECK(f.run("pkg list --installed").empty());
        CHECK(contains(f.run("pkg list 'b*'"), "busybox"));
        CHECK_FALSE(contains(f.run("pkg list 'b*'"), "lib"));
        CHECK(contains(f.run("pkg search UNIX"), "busybox"));
        CHECK(f.run("pkg search nothing-like-it", Shell::ExitFailure).empty());
        CHECK(contains(f.run("pkg info app"), "Depends:  lib (>= 2)"));

        // A name: its newest, and what it needs, from the lists.
        const std::string out = f.run("pkg install -y set");
        CHECK(contains(out, "lib 2: installed (a dependency)"));
        CHECK(contains(out, "set 1: installed\n"));
        CHECK(get("/t/root/share/lib") == "lib 2");
        const std::string again = f.run("pkg install -y set");
        CHECK(contains(again, "set: 1 is installed, the newest"));
        CHECK(contains(again, "nothing to do"));
        CHECK(contains(f.run("pkg upgrade"), "nothing to upgrade"));

        // The folder's own index, when it lists exactly its packages.
        CHECK(contains(f.run("pkg index /t/repo"), "/t/repo/index.bmpi: 5 packages"));
        CHECK(contains(f.run("pkg update"), "file:/t/repo: 5 packages\n"));

        // A newer one: upgradable, upgraded keeping its mark; the folder's index is out of date.
        make({"lib", "3", "", {"share"}, {{"share/lib", "lib 3"}}, {}}, "/t/repo");
        CHECK(contains(f.run("pkg update"), "file:/t/repo: 6 packages (indexed here"));
        CHECK(lineOf(f.run("pkg list --upgradable"), "lib") == "lib 2 -> 3 upgradable ");
        const std::string json = f.run("pkg --json list lib");
        CHECK(contains(json, "\"installed\":true,\"automatic\":true"));
        CHECK(contains(json, "\"available\":\"3\",\"upgradable\":true,\"keyName\":\"the test key\"}"));
        const std::string plan = f.run("pkg upgrade --dry-run");
        CHECK(contains(plan, "upgrade lib 3 (from 2), 5 bytes, signed by the test key (a built-in trusted key)\n"));
        CHECK(contains(f.run("pkg upgrade -y"), "lib 3: installed\n"));
        CHECK(get("/t/root/share/lib") == "lib 3");
        CHECK(lineOf(f.run("pkg list lib"), "lib") == "lib 3 auto ");
        CHECK(f.run("pkg list --upgradable").empty());

        // A list out of date: a file gone, or not what the list says.
        f.run("pkg remove -y set app");
        REQUIRE(Vfs::remove(sv(r.app), Root).isOk());
        CHECK(contains(f.run("pkg install -y app", Shell::ExitFailure),
                       "/t/repo/app_1.bmpk is gone: pkg update reads the lists again"));
        make({"app", "1", "depends\tlib (>= 2)\n", {"bin"}, {{"bin/app", "app"}}, {}}, "/t/repo");
        f.run("pkg update");
        put(r.app, build({"app", "9", "", {}, {}, {}}));
        CHECK(contains(f.run("pkg install -y app", Shell::ExitFailure),
                       "/t/repo/app_1.bmpk holds app 9, not what the list says: pkg update reads the lists again"));

        // JSON update records; clean empties the cache.
        CHECK(contains(f.run("pkg --json update"),
                       "{\"type\":\"source\",\"source\":\"file:/t/repo\",\"packages\":6,\"indexedHere\":true}"));
        REQUIRE(exists("/t/root/var/cache/pkg/lists"));
        f.run("pkg clean");
        CHECK_FALSE(contains(f.run("pkg list"), "available"));
}

TEST_CASE("Packages: --source, one source more for one command") {
        Fixture f;
        Repo    r;
        sources = "file:/t/none"; // configured, and missing
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "/t/none: "));
        CHECK_FALSE(contains(f.run("pkg list"), "available"));
        CHECK(contains(f.run("pkg --source file:/t/repo update", Shell::ExitFailure), "file:/t/repo: 5 packages"));
        CHECK(lineOf(f.run("pkg --source file:/t/repo list"), "set") == "set 1 available The set");
        CHECK(contains(f.run("pkg --source file:/t/repo install -y set"), "set 1: installed\n"));
        // Not after: the configured sources alone.
        CHECK_FALSE(contains(f.run("pkg list"), "available"));
        CHECK(lineOf(f.run("pkg list --installed"), "set") == "set 1 installed The set");
        CHECK(f.run("pkg --source", Shell::ExitUsage).rfind("usage: pkg", 0) == 0);
}

TEST_CASE("Packages: a dry run leaves the data's hash to the install") {
        Fixture     f;
        Repo        r;
        std::string bytes = get(r.lib2);
        bytes.back() = char(bytes.back() ^ 0x55); // in the data, the last section
        put(r.lib2, bytes);
        f.run("pkg update");
        // Planned and checked but for the data, which only installing reads.
        CHECK(contains(f.run("pkg install --dry-run lib"), "lib 2: would install"));
        CHECK(contains(f.run("pkg install -y lib", Shell::ExitFailure), "damaged (a hash doesn't match)"));
        CHECK(f.run("pkg list --installed").empty());
        // --json with -y says its plan first too.
        put(r.lib2, build({"lib", "2", "", {"share"}, {{"share/lib", "lib 2"}}, {}}));
        const std::string json = f.run("pkg --json install -y app");
        CHECK(json.find("{\"type\":\"plan\",\"action\":\"install\",\"name\":\"lib\"") <
              json.find("{\"type\":\"done\""));
        CHECK(contains(json, "{\"type\":\"plan\",\"action\":\"install\",\"name\":\"app\""));
}

TEST_CASE("Packages: sources checked, a list's icons, and a folder's packages left out") {
        bmds::String why;
        CHECK(Packages::checkSources("file:/sd0/p0/packages", why).isOk());
        CHECK(Packages::checkSources(" file:/a ; ;file:/b ", why).isOk());
        CHECK(Packages::checkSources("", why).isOk());
        CHECK(Packages::checkSources("file:a", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("file:/a/../b", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("https://example.com/packages", why).isOk());

        Fixture f;
        REQUIRE(Vfs::mkdir("/t/repo", Root).isOk());
        put("/t/repo/hello_2.0~rc1-3.bmpk", fixture("hello_2.0~rc1-3.bmpk"));
        put("/t/repo/junk_1.bmpk", "not a package");
        make(Busybox, "/t/repo");
        make(Tzdata, "/t/repo");
        const std::string out = f.run("pkg index /t/repo");
        CHECK(contains(out, "junk_1.bmpk: /t/repo/junk_1.bmpk: not a package"));
        CHECK(contains(out, "/t/repo/index.bmpi: 3 packages"));
        f.run("pkg update");
        // The icon of a package not installed, from the list; the description from its file.
        f.run("pkg icon hello --size 16 -o /t/icon.png");
        CHECK(uint8_t(get("/t/icon.png")[19]) == 16);
        CHECK(contains(f.run("pkg info hello"), "the dev key"));
        f.run("pkg install -y hello");
        CHECK(lineOf(f.run("pkg list"), "tzdata") == "tzdata 2025b-1 auto ");
}

namespace {

        std::string publicHex(const uint8_t (&seed)[32]) {
                bmds::SignedContainer::Key k;
                uint8_t                    unused[64];
                bmds::Ed25519::sign(seed, "", 0, unused, k.bytes);
                return hexOf(std::string(reinterpret_cast<const char *>(k.bytes), 32));
        }

        std::string idOf(const uint8_t (&seed)[32]) {
                bmds::SignedContainer::Key k;
                uint8_t                    unused[64];
                bmds::Ed25519::sign(seed, "", 0, unused, k.bytes);
                uint8_t id[8];
                bmds::SignedContainer::keyId(k, id);
                return hexOf(std::string(reinterpret_cast<const char *>(id), 8));
        }

} // namespace

TEST_CASE("Packages: the keyring, keys added and removed, who signed said") {
        Fixture           f;
        const std::string other = idOf(OtherSeed);
        const std::string list = f.run("pkg key list");
        CHECK(contains(list, idOf(Seed) + "  trusted  the test key\n"));
        CHECK(contains(list, "1edc0f2cc301cc37  dev      the dev key's comment\n"));

        // Signed by a key not in the keyring: refused, until it is added.
        Spec              theirs = {"theirs", "1", "", {"share"}, {{"share/theirs", "theirs"}}, {}};
        const std::string path = "/t/theirs_1.bmpk";
        put(path, build(theirs, OtherSeed));
        CHECK(contains(f.run("pkg install -y " + path, Shell::ExitFailure),
                       "theirs: not signed by a key this system accepts"));
        put("/t/other.pub", "# their key\ned25519 " + publicHex(OtherSeed) + " Someone else's packages\n");
        CHECK(contains(f.run("pkg key add /t/other.pub"), other + ": added: Someone else's packages"));
        CHECK(get("/t/keys/" + other + ".pub") == "ed25519 " + publicHex(OtherSeed) + " Someone else's packages\n");
        CHECK(contains(f.run("pkg key list"), other + "  added    Someone else's packages"));
        CHECK(contains(f.run("pkg install --dry-run " + path), "signed by Someone else's packages (an added key)"));
        f.run("pkg install -y " + path);
        CHECK(contains(f.run("pkg info theirs"),
                       "Signed:   Someone else's packages (an added key; key " + other + ")"));
        CHECK(contains(f.run("pkg --json info theirs"), "\"signer\":\"added\""));

        // What add and remove refuse.
        CHECK(contains(f.run("pkg key add /t/other.pub", Shell::ExitFailure),
                       other + " is in the keyring already (added)"));
        put("/t/dev.pub", "ed25519 " + hexOf(std::string(reinterpret_cast<const char *>(keys[1].bytes), 32)) + " x\n");
        CHECK(contains(f.run("pkg key add /t/dev.pub", Shell::ExitFailure), "is the dev key"));
        put("/t/none.pub", "nothing here\n");
        CHECK(contains(f.run("pkg key add /t/none.pub", Shell::ExitFailure), "no key in it"));
        CHECK(contains(f.run("pkg key remove " + idOf(Seed), Shell::ExitFailure), "built into the firmware"));
        CHECK(contains(f.run("pkg key remove 12", Shell::ExitFailure), "4 to 16 hex digits"));
        CHECK(contains(f.run("pkg key remove ffffffff", Shell::ExitFailure), "no such key"));

        // A dev key put in the folder by hand is skipped.
        put("/t/keys/sneaky.pub", get("/t/dev.pub"));
        CHECK_FALSE(contains(f.run("pkg key list"), "added    x"));

        // Removed: what it signed is installed still, and said so; nothing more of it installs.
        CHECK(contains(f.run("pkg key remove " + other.substr(0, 6)), other + ": removed: Someone else's packages"));
        CHECK_FALSE(exists("/t/keys/" + other + ".pub"));
        CHECK(contains(f.run("pkg info theirs"), "Signed:   key " + other + " (no longer in the keyring; key "));
        CHECK(contains(f.run("pkg install -y " + path, Shell::ExitFailure), "not signed by a key this system accepts"));
        CHECK(contains(f.run("pkg --json key list"), "{\"type\":\"key\",\"id\":\"" + idOf(Seed) +
                                                             "\",\"kind\":\"trusted\",\"comment\":\"the test key\"}"));
}

TEST_CASE("Packages: pkg/dev, allow, ask and refuse") {
        Fixture           f;
        const std::string hello = "/t/hello_2.0~rc1-3.bmpk";
        put(hello, fixture("hello_2.0~rc1-3.bmpk"));
        const std::string deps = make(Busybox) + " " + make(Tzdata);

        devPolicy = Packages::DevPolicy::Refuse;
        CHECK(contains(f.run("pkg install -y " + deps + " " + hello, Shell::ExitFailure),
                       "hello: signed by the dev key (anyone could have), which the setting pkg/dev refuses"));
        CHECK(f.run("pkg list --installed").empty());

        // Ask: its own question, after the plan's; -y answers both.
        devPolicy = Packages::DevPolicy::Ask;
        f.con.type("y\nn\n");
        const std::string no = f.run("pkg install " + deps + " " + hello, Shell::ExitFailure);
        CHECK(contains(no, "Signed by the dev key, so anyone could have made them: hello\nInstall them anyway?"));
        CHECK(f.run("pkg list --installed").empty());
        f.con.type("y\ny\n");
        CHECK(contains(f.run("pkg install " + deps + " " + hello), "hello 1:2.0~rc1-3: installed"));
        f.run("pkg remove -y hello");
        CHECK(contains(f.run("pkg install -y " + hello), "hello 1:2.0~rc1-3: installed"));
        // Packages trusted keys signed need no question.
        f.run("pkg remove -y hello busybox tzdata");
        f.con.type("y\n");
        CHECK_FALSE(contains(f.run("pkg install " + deps), "Install them anyway?"));
}

TEST_CASE("Packages: a source that names its keys") {
        Fixture f;
        REQUIRE(Vfs::mkdir("/t/repo", Root).isOk());
        put("/t/repo/hello_2.0~rc1-3.bmpk", fixture("hello_2.0~rc1-3.bmpk"));
        put("/t/repo/bmds-standard_0.1.0-1.bmpk", fixture("bmds-standard_0.1.0-1.bmpk"));
        make(Busybox, "/t");
        make(Tzdata, "/t");

        // No index: the board can't make one that counts.
        sources = "file:/t/repo signed-by=1edc0f2cc301cc37";
        CHECK(contains(f.run("pkg update", Shell::ExitFailure),
                       "/t/repo: its index.bmpi is missing or out of date, and the source needs one signed by "
                       "1edc0f2cc301cc37 (mkpkg index --key signs)"));
        f.run("pkg index /t/repo");
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "isn't signed by 1edc0f2cc301cc37"));

        // mkpkg's, signed by the dev key: taken; by another key: not.
        put("/t/repo/index.bmpi", fixture("index.bmpi"));
        CHECK(contains(f.run("pkg update"), "file:/t/repo signed-by=1edc0f2cc301cc37: 2 packages\n"));
        CHECK(lineOf(f.run("pkg list"), "hello") == "hello 1:2.0~rc1-3 available Says hello");
        sources = "file:/t/repo signed-by=" + idOf(Seed);
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "isn't signed by " + idOf(Seed)));
        CHECK_FALSE(contains(f.run("pkg list"), "hello")); // the list of the source as it was went

        // A package must hash as its signed list says.
        sources = "file:/t/repo signed-by=1edc0f2cc301cc37";
        f.run("pkg update");
        f.run("pkg install -y /t/busybox_1.37.0-1.bmpk /t/tzdata_2025b-1.bmpk");
        std::string tampered = fixture("hello_2.0~rc1-3.bmpk");
        tampered[tampered.size() - 1] ^= 1; // in the data: a peek doesn't see it
        put("/t/repo/hello_2.0~rc1-3.bmpk", tampered);
        CHECK(contains(f.run("pkg install -y hello", Shell::ExitFailure),
                       "/t/repo/hello_2.0~rc1-3.bmpk doesn't match its signed list's hash"));
        put("/t/repo/hello_2.0~rc1-3.bmpk", fixture("hello_2.0~rc1-3.bmpk"));
        CHECK(contains(f.run("pkg install -y hello"), "hello 1:2.0~rc1-3: installed"));

        // What the setting refuses.
        bmds::String why;
        CHECK(Packages::checkSources("file:/a signed-by=1edc0f2cc301cc37,0123456789abcdef", why).isOk());
        CHECK(Packages::checkSources("file:/a signed-by=", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("file:/a signed-by=1EDC0F2CC301CC37", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("file:/a trusted=yes", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("file:/a signed-by=1edc0f2cc301cc37 signed-by=1edc0f2cc301cc37", why).code() ==
              Error::Invalid);
}

namespace {

        // An index of @p packages (files in @p dir, listed by name), signed with @p seed.
        std::string signedIndex(const std::vector<std::string> &packages, const std::string &dir,
                                const uint8_t (&seed)[32]) {
                std::string stanzas;
                for (const std::string &name : packages) {
                        const std::string bytes = get(dir + "/" + name);
                        Packages::Info    info;
                        bmds::String      why;
                        REQUIRE(Packages::peekFile(sv(dir + "/" + name), config(), info, why).isOk());
                        stanzas +=
                                (stanzas.empty() ? "" : "\n") + std::string("Package: ") +
                                std::string(info.name.data(), info.name.size()) +
                                "\nVersion: " + std::string(info.version.data(), info.version.size()) +
                                "\nArchitecture: " + std::string(info.arch.data(), info.arch.size()) + "\nABI: 1\n" +
                                (info.depends.isEmpty()
                                         ? ""
                                         : "Depends: " + std::string(info.depends.data(), info.depends.size()) + "\n") +
                                "Size: " + std::to_string(bytes.size()) +
                                "\nInstalled-Size: " + std::to_string(info.installedBytes) +
                                "\nBLAKE2s: " + hexOf(blake2s(bytes)) + "\nFile: " + name + "\n";
                }
                bmds::List<uint8_t> out;
                REQUIRE(bmds::PkgFile::buildIndex(sv(stanzas), bmds::ByteSpan(), uint32_t(packages.size()), 0, "web",
                                                  out)
                                .isOk());
                std::string index(reinterpret_cast<const char *>(out.data()), out.size());
                // Signed: the key's id in the prefix, then the signature over the header and the table.
                bmds::SignedContainer::Key k;
                uint8_t                    sig[64];
                bmds::Ed25519::sign(seed, "", 0, sig, k.bytes);
                uint8_t id[8];
                bmds::SignedContainer::keyId(k, id);
                index.replace(16, 8, reinterpret_cast<const char *>(id), 8);
                const size_t signedBytes = 72 + 48 * 1;
                bmds::Ed25519::sign(seed, index.data(), signedBytes, sig, k.bytes);
                index.replace(signedBytes, 64, reinterpret_cast<const char *>(sig), 64);
                return index;
        }

        // The web's repository: app (needing lib >= 2) and lib 2, its index signed by the test key.
        void webRepo(const uint8_t (&seed)[32] = Seed) {
                (void)Vfs::mkdir("/t/web", Root);
                (void)Vfs::mkdir("/t/web/repo", Root);
                make({"app", "1", "depends\tlib (>= 2)\n", {"bin"}, {{"bin/app", "app"}}, {}}, "/t/web/repo");
                make({"lib", "2", "", {"share"}, {{"share/lib", "lib 2"}}, {}}, "/t/web/repo");
                put("/t/web/repo/index.bmpi", signedIndex({"app_1.bmpk", "lib_2.bmpk"}, "/t/web/repo", seed));
                return;
        }

} // namespace

TEST_CASE("Packages: a network source, its packages fetched, kept, resumed") {
        Fixture f;
        webRepo();
        sources = "http://web/repo";
        CHECK(contains(f.run("pkg update"), "http://web/repo: 2 packages\n"));
        CHECK(lineOf(f.run("pkg list"), "app") == "app 1 available ");

        // The plan says what will be fetched, and from where; a dry run fetches nothing.
        const int         before = web.calls;
        const std::string plan = f.run("pkg install --dry-run app");
        CHECK(contains(plan, "install lib 2, 5 bytes, " + std::to_string(get("/t/web/repo/lib_2.bmpk").size()) +
                                     " to fetch from web (a dependency)"));
        CHECK(contains(plan, "app 1: would install\n"));
        CHECK(web.calls == before);
        CHECK(contains(f.run("pkg --json install --dry-run app"), "\"signer\":\"\",\"automatic\":true,\"file\":"
                                                                  "\"http://web/repo/lib_2.bmpk\",\"fetch\":"));

        const std::string out = f.run("pkg install -y app");
        CHECK(contains(out, "fetching lib 2 ("));
        CHECK(contains(out, "lib 2: installed (a dependency)"));
        CHECK(contains(out, "app 1: installed\n"));
        CHECK(get("/t/root/bin/app") == "app");
        CHECK(exists("/t/root/var/cache/pkg/packages/lib_2.bmpk"));
        CHECK(contains(f.run("pkg info lib"), "Signed:   the test key (a built-in trusted key"));

        // Kept: installing again fetches nothing.
        f.run("pkg remove -y app lib");
        const int kept = web.calls;
        f.run("pkg install -y app");
        CHECK(web.calls == kept);

        // Cut short: the .part stays, and the next fetch resumes it.
        f.run("pkg remove -y app lib");
        f.run("pkg clean");
        CHECK_FALSE(exists("/t/root/var/cache/pkg/packages/lib_2.bmpk"));
        f.run("pkg update");
        web.cutAfter = 100;
        CHECK(contains(f.run("pkg install -y app", Shell::ExitFailure),
                       "lib: http://web/repo/lib_2.bmpk: the connection ended early"));
        CHECK(get("/t/root/var/cache/pkg/packages/lib_2.bmpk.part").size() == 100);
        web.froms.clear();
        f.run("pkg install -y app");
        CHECK(get("/t/root/share/lib") == "lib 2");
        REQUIRE(web.froms.size() == 2);
        CHECK(web.froms[0] == 100); // lib's, resumed
        CHECK(web.froms[1] == 0);   // app's, whole

        // A server that ignores the range sends it all again: still right.
        f.run("pkg remove -y app lib");
        f.run("pkg clean");
        f.run("pkg update");
        web.cutAfter = 50;
        f.run("pkg install -y lib", Shell::ExitFailure);
        web.ignoreRange = true;
        f.run("pkg install -y lib");
        CHECK(get("/t/root/share/lib") == "lib 2");
}

TEST_CASE("Packages: what a network source refuses") {
        Fixture f;
        webRepo();
        sources = "http://web/repo";
        // A package changed since its index was signed.
        f.run("pkg update");
        const std::string lib = get("/t/web/repo/lib_2.bmpk");
        std::string       changed = lib;
        changed[changed.size() - 1] ^= 1;
        put("/t/web/repo/lib_2.bmpk", changed);
        CHECK(contains(f.run("pkg install -y lib", Shell::ExitFailure),
                       "http://web/repo/lib_2.bmpk doesn't match its signed list"));
        CHECK_FALSE(exists("/t/root/var/cache/pkg/packages/lib_2.bmpk.part"));
        put("/t/web/repo/lib_2.bmpk", lib);

        // An index unsigned, or signed by a key not in the keyring, or not by the keys named.
        f.run("pkg index /t/web/repo");
        CHECK(contains(f.run("pkg update", Shell::ExitFailure),
                       "http://web/repo/index.bmpi: unsigned (a network source's index must be signed)"));
        put("/t/web/repo/index.bmpi", signedIndex({"app_1.bmpk", "lib_2.bmpk"}, "/t/web/repo", OtherSeed));
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "not signed by a key in the keyring (pkg key list)"));
        put("/t/web/repo/index.bmpi", signedIndex({"app_1.bmpk", "lib_2.bmpk"}, "/t/web/repo", Seed));
        sources = "http://web/repo signed-by=1edc0f2cc301cc37";
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "not signed by the keys the source names"));

        // Nothing there; no fetching at all.
        sources = "http://web/nothing";
        CHECK(contains(f.run("pkg update", Shell::ExitFailure),
                       "pkg: http://web/nothing/index.bmpi: the server answered 404 (not found)\n"));
        web.on = false;
        sources = "http://web/repo";
        CHECK(contains(f.run("pkg update", Shell::ExitFailure), "http://web/repo: no fetching in this system"));

        bmds::String why;
        CHECK(Packages::checkSources("https://example.com/bmds/packages signed-by=1edc0f2cc301cc37; file:/a", why)
                      .isOk());
        CHECK(Packages::checkSources("https://bad host/x", why).code() == Error::Invalid);
        CHECK(Packages::checkSources("ftp://example.com/x", why).code() == Error::Invalid);
}
