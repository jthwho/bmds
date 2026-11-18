/**
 * @file      romeditor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ROM changed (devplan/rom.md §12b, §12d, §12g): RomEditor on a simulated NOR flash (writes
 * only clear bits, erases set 0xff, the space past the image holds old bytes) with power cut at
 * every operation of a change. Files added, replaced and removed, the pending tree discarded,
 * files moved together, an update merged; the booted tree's bytes never written; every cut
 * leaves the old tree or the new.
 */

#include <doctest.h>

#ifdef BMDS_TEST_ROM

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <bmds/rommanifest.h>
#include <bmds/romeditor.h>
#include <bmds/romspace.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::RomEditor;
using bmds::RomImage;
using bmds::StringView;
using bmds::WritableByteSpan;

namespace {

        constexpr uint32_t Sector = RomImage::SectorBytes;

        // A NOR flash in memory: writes clear bits only, erases set whole sectors to 0xff; a cut
        // stops it at an operation (half of that one done), and every one after fails.
        class SimFlash final : public bmds::RomFlash {
                public:
                        std::vector<uint8_t> mem;
                        long                 opsLeft = -1; // -1: no cut
                        long                 ops = 0;
                        size_t               badWrites = 0; // bytes written that weren't erased

                        ByteSpan bytes() const override { return ByteSpan(mem.data(), mem.size()); }

                        Error erase(uint32_t offset, uint32_t bytes) override {
                                REQUIRE(offset % Sector == 0);
                                REQUIRE(bytes % Sector == 0);
                                REQUIRE(uint64_t(offset) + bytes <= mem.size());
                                if (cut()) {
                                        std::fill(mem.begin() + offset, mem.begin() + offset + bytes / 2, 0xff);
                                        return Error::IoError;
                                }
                                std::fill(mem.begin() + offset, mem.begin() + offset + bytes, 0xff);
                                return Error::Ok;
                        }

                        Error write(uint32_t offset, ByteSpan data) override {
                                REQUIRE(uint64_t(offset) + data.size() <= mem.size());
                                const bool   stopped = cut();
                                const size_t n = stopped ? data.size() / 2 : data.size();
                                for (size_t i = 0; i < n; ++i) {
                                        if ((mem[offset + i] & data[i]) != data[i]) ++badWrites;
                                        mem[offset + i] &= data[i];
                                }
                                return stopped ? Error::IoError : Error::Ok;
                        }

                        Error read(uint32_t offset, WritableByteSpan out) override {
                                if (opsLeft == 0) return Error::IoError;
                                std::memcpy(out.data(), mem.data() + offset, out.size());
                                return Error::Ok;
                        }

                private:
                        bool cut() {
                                ++ops;
                                if (opsLeft < 0) return false;
                                if (opsLeft == 0) return true;
                                return --opsLeft == 0;
                        }
        };

        std::string readHost(const char *path) {
                std::ifstream     in(path, std::ios::binary);
                std::stringstream s;
                s << in.rdbuf();
                return s.str();
        }

        // The test ROM in a 3 MB partition, the space past it holding old bytes (not erased).
        SimFlash freshFlash() {
                SimFlash          f;
                const std::string image = readHost(BMDS_TEST_ROM);
                f.mem.assign(image.begin(), image.end());
                f.mem.resize(3 * 1024 * 1024, 0x5a);
                return f;
        }

        RomImage openTree(const SimFlash &f, bmds::Span<const uint8_t> firmware = {}) {
                auto [image, err] = RomImage::open(f.bytes(), firmware);
                REQUIRE(err.isOk());
                return image;
        }

        std::string text(ByteSpan b) {
                return std::string(reinterpret_cast<const char *>(b.data()), b.size());
        }

        // The file at @p path in @p tree ("" if none).
        std::string fileText(const RomImage &tree, const char *path) {
                bool       noPath = false;
                const auto found = tree.find(path, noPath);
                if (found.error().isError() || found.value().directory) return "";
                return text(tree.entry(found.value().index).data);
        }

        bool has(const RomImage &tree, const char *path) {
                bool noPath = false;
                return tree.find(path, noPath).error().isOk();
        }

        // The booted tree's bytes: its files and its version (which a change must never write).
        std::string bootedBytes(const SimFlash &f, const RomImage &booted) {
                std::string out(reinterpret_cast<const char *>(f.mem.data()) + booted.version().offset,
                                booted.version().bytes);
                for (uint32_t i = 0; i < booted.count(); ++i) out += text(booted.entry(i).data);
                return out;
        }

        struct Bytes final : RomEditor::Source {
                        std::string data;
                        size_t      at = 0;
                        explicit Bytes(std::string d) : data(std::move(d)) {}
                        uint64_t             size() const override { return data.size(); }
                        bmds::Result<size_t> read(WritableByteSpan buf) override {
                                const size_t n = std::min(buf.size(), data.size() - at);
                                std::memcpy(buf.data(), data.data() + at, n);
                                at += n;
                                return bmds::makeResult(n);
                        }
        };

        Error installOne(RomEditor &editor, const char *path, const std::string &data, uint8_t alignLog2 = 6,
                         uint8_t flags = RomImage::User) {
                Bytes                source(data);
                const RomEditor::Add add{path, &source, alignLog2, flags, bmds::Timestamp::fromSeconds(1800000000)};
                RomEditor::Report    report;
                return editor.install(bmds::Span<const RomEditor::Add>(&add, 1), report);
        }

        std::string pattern(size_t n, int seed) {
                std::string s(n, '\0');
                for (size_t i = 0; i < n; ++i) s[i] = char((i * 131 + seed * 7 + (i >> 8)) & 0xff);
                return s;
        }

        size_t holes(const RomImage &booted, const RomImage *pending) {
                bmds::RomSpace space;
                REQUIRE(space.build(booted, pending).isOk());
                CHECK(space.summary().overlaps == 0);
                return space.summary().holeBytes;
        }

} // namespace

TEST_CASE("rom editor: files added, replaced and removed take effect as a pending tree; the booted one untouched") {
        SimFlash          f = freshFlash();
        const RomImage    booted = openTree(f);
        const std::string before = bootedBytes(f, booted);
        RomEditor         editor(f, &booted, {});
        REQUIRE(editor.load().isOk());
        CHECK_FALSE(editor.hasPending());

        // One file, then a batch: a small one, one on a page, one aligned to 4 KB, a link's flag.
        REQUIRE(installOne(editor, "bin/hello", "#!/bin/env sh\necho hi\n").isOk());
        CHECK(editor.hasPending());
        Bytes                a(pattern(100000, 1)), b(pattern(70000, 2)), c(pattern(5000, 3));
        const RomEditor::Add batch[] = {
                {"share/x/one.bin", &a, 12, RomImage::User, {}},
                {"share/x/page.bin", &b, 16, RomImage::User, {}},
                {"B.txt", &c, 6, RomImage::User, {}}, // replaces the system's
        };
        RomEditor::Report report;
        REQUIRE(editor.install(bmds::Span<const RomEditor::Add>(batch, 3), report).isOk());
        CHECK(report.written == 3);
        CHECK(report.bytesWritten == 175000);
        CHECK(report.sequence == 3);
        CHECK(f.badWrites == 0);

        const RomImage pending = openTree(f);
        CHECK(pending.version().sequence == 3);
        CHECK(pending.verify().isOk());
        CHECK(fileText(pending, "bin/hello") == "#!/bin/env sh\necho hi\n");
        CHECK(fileText(pending, "share/x/one.bin") == pattern(100000, 1));
        CHECK(fileText(pending, "B.txt") == pattern(5000, 3));
        bool       noPath = false;
        const auto page = pending.entry(pending.find("share/x/page.bin", noPath).value().index);
        CHECK(page.offset % RomImage::PageBytes == 0);
        CHECK(page.offset >= f.mem.size() - 2 * RomImage::PageBytes); // from the top down
        CHECK(pending.entry(pending.find("share/x/one.bin", noPath).value().index).offset % 4096 == 0);
        CHECK((pending.entry(pending.find("B.txt", noPath).value().index).flags & RomImage::User) != 0);
        // The booted tree: the same bytes, its files as they were.
        CHECK(bootedBytes(f, booted) == before);
        CHECK(booted.verify().isOk());
        CHECK_FALSE(has(booted, "bin/hello"));
        // The manifest of the booted tree's files now sees B.txt replaced.
        // (The pending tree against the booted: three added or replaced.)
        CHECK(bmds::RomManifest::compareTrees(booted, pending, nullptr, nullptr) == 4);

        // Removed: a file, a directory (not without -r), one missing.
        REQUIRE(editor.load().isOk());
        const StringView dir[] = {"share/x"};
        CHECK(editor.remove(bmds::Span<const StringView>(dir, 1), false, report) == Error::IsDir);
        CHECK(editor.failedPath() == "share/x");
        const StringView missing[] = {"nothing"};
        CHECK(editor.remove(bmds::Span<const StringView>(missing, 1), true, report) == Error::NotExist);
        const StringView gone[] = {"share/x", "a.txt"};
        REQUIRE(editor.remove(bmds::Span<const StringView>(gone, 2), true, report).isOk());
        CHECK(report.removed == 3);
        const RomImage after = openTree(f);
        CHECK_FALSE(has(after, "share/x"));
        CHECK_FALSE(has(after, "a.txt"));
        CHECK(has(after, "bin/hello"));
        CHECK(after.verify().isOk());
        CHECK(bootedBytes(f, booted) == before);

        // Discarded: the booted tree's files again.
        REQUIRE(editor.discard(report).isOk());
        CHECK(report.sequence == 5);
        const RomImage back = openTree(f);
        CHECK(bmds::RomManifest::compareTrees(booted, back, nullptr, nullptr) == 0);
        CHECK(back.verify().isOk());
        CHECK(f.badWrites == 0);
}

TEST_CASE("rom editor: refusals change nothing") {
        SimFlash       f = freshFlash();
        const RomImage booted = openTree(f);
        RomEditor      editor(f, &booted, {});
        REQUIRE(editor.load().isOk());
        const std::vector<uint8_t> was = f.mem;
        CHECK(installOne(editor, "docs", "x") == Error::Invalid); // a directory there
        CHECK(editor.failedPath() == "docs");
        CHECK(installOne(editor, "hello.txt/x", "x") == Error::Invalid); // under a file
        CHECK(editor.failedPath() == "hello.txt/x");
        CHECK(installOne(editor, "/abs", "x") == Error::Invalid);
        CHECK(installOne(editor, "a/../b", "x") == Error::Invalid);
        Bytes                one("1"), two("2");
        const RomEditor::Add twice[] = {{"same", &one, 6, RomImage::User, {}}, {"same", &two, 6, RomImage::User, {}}};
        RomEditor::Report    report;
        CHECK(editor.install(bmds::Span<const RomEditor::Add>(twice, 2), report) == Error::Invalid);
        // Larger than the space: NoSpace, and why.
        CHECK(installOne(editor, "big", std::string(4 * 1024 * 1024, 'x')) == Error::NoSpace);
        CHECK(editor.shortfall().needed == 4u * 1024 * 1024);
        CHECK(editor.shortfall().freeNow > 0);
        CHECK(editor.shortfall().largestNow <= editor.shortfall().freeNow);
        CHECK(editor.shortfall().freeAfterBoot >= editor.shortfall().freeNow);
        CHECK(f.mem == was);
        CHECK(openTree(f).version().sequence == 1);
}

TEST_CASE("rom editor: space freed only by the booted tree waits for the next boot; a pending file's at once") {
        SimFlash  f = freshFlash();
        RomImage  booted = openTree(f);
        RomEditor editor(f, &booted, {});
        REQUIRE(editor.load().isOk());
        bool noPath = false;
        REQUIRE(installOne(editor, "one", pattern(8000, 1)).isOk());
        const uint32_t first = openTree(f).entry(openTree(f).find("one", noPath).value().index).offset;
        REQUIRE(installOne(editor, "one", pattern(8000, 2)).isOk()); // replaced while pending
        const uint32_t second = openTree(f).entry(openTree(f).find("one", noPath).value().index).offset;
        CHECK(second != first);
        // The first copy was in no booted tree: free now, the lowest run, taken next.
        REQUIRE(installOne(editor, "two", pattern(6000, 3)).isOk());
        CHECK(openTree(f).entry(openTree(f).find("two", noPath).value().index).offset == first);

        // "two" removed: free after the next boot only once it is booted.
        const StringView  two[] = {"two"};
        RomEditor::Report report;
        REQUIRE(editor.remove(bmds::Span<const StringView>(two, 1), false, report).isOk());
        REQUIRE(installOne(editor, "three", pattern(6000, 4)).isOk());
        CHECK(openTree(f).entry(openTree(f).find("three", noPath).value().index).offset == first); // pending-only: free

        // A reboot: the pending tree is booted. Its "one" removed: not free until the next.
        booted = openTree(f);
        RomEditor rebooted(f, &booted, {});
        REQUIRE(rebooted.load().isOk());
        CHECK_FALSE(rebooted.hasPending());
        const StringView one[] = {"one"};
        REQUIRE(rebooted.remove(bmds::Span<const StringView>(one, 1), false, report).isOk());
        REQUIRE(installOne(rebooted, "four", pattern(8000, 5)).isOk());
        CHECK(openTree(f).entry(openTree(f).find("four", noPath).value().index).offset != second);
        CHECK(f.badWrites == 0);
}

TEST_CASE("rom editor: many changes in a row: the log's sectors reused, the booted version kept") {
        SimFlash          f = freshFlash();
        const RomImage    booted = openTree(f);
        const std::string before = bootedBytes(f, booted);
        RomEditor         editor(f, &booted, {});
        REQUIRE(editor.load().isOk());
        RomEditor::Report report;
        for (int i = 0; i < 60; ++i) {
                const std::string path = "n" + std::to_string(i % 5);
                REQUIRE(installOne(editor, path.c_str(), pattern(3000 + i, i)).isOk());
        }
        const RomImage now = openTree(f);
        CHECK(now.version().sequence == 61);
        CHECK(now.verify().isOk());
        CHECK(fileText(now, "n4") == pattern(3059, 59));
        CHECK(bootedBytes(f, booted) == before);
        CHECK(f.badWrites == 0);
}

TEST_CASE("rom editor: a power cut at any operation leaves the old tree or the new, whole") {
        // Each change, run once to count its operations, then cut at each.
        enum class Change {
                Install,
                Remove,
                Discard,
                Compact,
                Apply
        };
        // An update's tree for Apply: B.txt changed, a.txt gone.
        SimFlash pkg = freshFlash();
        {
                RomEditor e(pkg, nullptr, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "B.txt", pattern(30000, 7), 6, 0).isOk());
                const StringView  gone[] = {"a.txt"};
                RomEditor::Report r;
                REQUIRE(e.remove(bmds::Span<const StringView>(gone, 1), false, r).isOk());
        }
        const RomImage package = openTree(pkg);
        for (Change change : {Change::Install, Change::Remove, Change::Discard, Change::Compact, Change::Apply}) {
                CAPTURE(int(change));
                // A starting point with something pending (and, for compaction, holes).
                SimFlash base = freshFlash();
                RomImage booted = openTree(base);
                {
                        RomEditor e(base, &booted, {});
                        REQUIRE(e.load().isOk());
                        for (int i = 0; i < 4; ++i)
                                REQUIRE(installOne(e, ("u" + std::to_string(i)).c_str(), pattern(9000, i)).isOk());
                        const StringView  gaps[] = {"u0", "u2"};
                        RomEditor::Report r;
                        REQUIRE(e.remove(bmds::Span<const StringView>(gaps, 2), false, r).isOk());
                }
                if (change == Change::Compact) booted = openTree(base); // the holes booted
                const RomImage committed = openTree(base);
                const uint32_t oldSequence = committed.version().sequence;
                auto           run = [&](SimFlash &f) -> Error {
                        RomEditor e(f, change == Change::Apply ? nullptr : &booted, {});
                        REQUIRE(e.load().isOk());
                        RomEditor::Report    r;
                        bool                 more = false;
                        Bytes                x(pattern(20000, 9)), y(pattern(70000, 8));
                        const RomEditor::Add adds[] = {{"u1", &x, 6, RomImage::User, {}},
                                                       {"p", &y, 16, RomImage::User, {}}};
                        const StringView     gone[] = {"u3", "hello.txt"};
                        switch (change) {
                                case Change::Install: return e.install(bmds::Span<const RomEditor::Add>(adds, 2), r);
                                case Change::Remove: return e.remove(bmds::Span<const StringView>(gone, 2), false, r);
                                case Change::Discard: return e.discard(r);
                                case Change::Compact: return e.compact(r, more);
                                case Change::Apply: return e.apply(package, {}, nullptr, nullptr, r);
                        }
                        return Error::Invalid;
                };
                SimFlash counted = base;
                REQUIRE(run(counted).isOk());
                const RomImage done = openTree(counted);
                REQUIRE(done.version().sequence == oldSequence + 1);
                const long total = counted.ops - base.ops; // this change's alone
                REQUIRE(total > 0);
                for (long k = 1; k <= total; ++k) {
                        CAPTURE(k);
                        SimFlash f = base;
                        f.opsLeft = k;
                        CHECK(run(f).isError());
                        const RomImage after = openTree(f);
                        const uint32_t seq = after.version().sequence;
                        CHECK((seq == oldSequence || seq == oldSequence + 1));
                        CHECK(after.verify().isOk());
                        // (In recovery nothing is booted: what only it used may be written.)
                        if (change != Change::Apply) CHECK(bootedBytes(f, booted) == bootedBytes(base, booted));
                        const RomImage &expect = seq == oldSequence ? committed : done;
                        CHECK(bmds::RomManifest::compareTrees(expect, after, nullptr, nullptr) == 0);
                }
        }
}

TEST_CASE("rom editor: compaction moves the files after the first hole together; two boots pack them down") {
        SimFlash f = freshFlash();
        RomImage booted = openTree(f);
        {
                RomEditor e(f, &booted, {});
                REQUIRE(e.load().isOk());
                for (int i = 0; i < 6; ++i)
                        REQUIRE(installOne(e, ("u" + std::to_string(i)).c_str(), pattern(9000, i)).isOk());
                const StringView  gaps[] = {"u1", "u3"};
                RomEditor::Report r;
                REQUIRE(e.remove(bmds::Span<const StringView>(gaps, 2), false, r).isOk());
        }
        booted = openTree(f); // rebooted: the holes are free
        CHECK(holes(booted, nullptr) > 0);
        for (int round = 0; round < 3; ++round) {
                RomEditor e(f, &booted, {});
                REQUIRE(e.load().isOk());
                RomEditor::Report r;
                bool              more = false;
                REQUIRE(e.compact(r, more).isOk());
                const RomImage pending = openTree(f);
                CHECK(pending.verify().isOk());
                for (int i : {0, 2, 4, 5})
                        CHECK(fileText(pending, ("u" + std::to_string(i)).c_str()) == pattern(9000, i));
                booted = openTree(f); // reboot
                if (r.moved == 0) break;
        }
        CHECK(holes(booted, nullptr) == 0);
        // Packed: compacting again moves nothing.
        RomEditor e(f, &booted, {});
        REQUIRE(e.load().isOk());
        RomEditor::Report r;
        bool              more = true;
        REQUIRE(e.compact(r, more).isOk());
        CHECK(r.moved == 0);
        CHECK_FALSE(more);
        CHECK(f.badWrites == 0);
}

TEST_CASE("rom editor: compaction fills a hole with the files from the top, moving no more than fills it") {
        SimFlash f = freshFlash();
        RomImage booted = openTree(f);
        {
                RomEditor e(f, &booted, {});
                REQUIRE(e.load().isOk());
                for (int i = 0; i < 6; ++i)
                        REQUIRE(installOne(e, ("u" + std::to_string(i)).c_str(), pattern(9000, i)).isOk());
                const StringView  gone[] = {"u0"}; // the lowest: a hole under all the others
                RomEditor::Report r;
                REQUIRE(e.remove(bmds::Span<const StringView>(gone, 1), false, r).isOk());
        }
        booted = openTree(f);
        bool           noPath = false;
        const uint32_t u5 = booted.entry(booted.find("u5", noPath).value().index).offset;
        const uint32_t u1 = booted.entry(booted.find("u1", noPath).value().index).offset;
        RomEditor      e(f, &booted, {});
        REQUIRE(e.load().isOk());
        RomEditor::Report r;
        bool              more = false;
        REQUIRE(e.compact(r, more).isOk());
        CHECK(r.moved == 1); // u5 alone, into u0's place
        const RomImage after = openTree(f);
        const uint32_t moved = after.entry(after.find("u5", noPath).value().index).offset;
        CHECK(moved < u1);
        CHECK(moved < u5);
        for (int i = 1; i < 5; ++i)
                CHECK(after.entry(after.find(("u" + std::to_string(i)).c_str(), noPath).value().index).offset ==
                      booted.entry(booted.find(("u" + std::to_string(i)).c_str(), noPath).value().index).offset);
        CHECK(fileText(after, "u5") == pattern(9000, 5));
        CHECK(after.verify().isOk());
        // After the boot the old place is at the top, with the free space: no holes.
        booted = openTree(f);
        CHECK(holes(booted, nullptr) == 0);
}

TEST_CASE("rom editor: an update merged in recovery: system files written where they differ, the user's kept") {
        // The update's tree: B.txt changed, a.txt gone, new.txt added (made by an editor with no
        // booted tree, as recovery is, from the test ROM).
        SimFlash pkg = freshFlash();
        {
                RomEditor e(pkg, nullptr, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "B.txt", "B, newer\n", 6, 0).isOk());
                REQUIRE(installOne(e, "new.txt", "new\n", 6, 0).isOk());
                const StringView  gone[] = {"a.txt"};
                RomEditor::Report r;
                REQUIRE(e.remove(bmds::Span<const StringView>(gone, 1), false, r).isOk());
        }
        const RomImage package = openTree(pkg);

        // The board: the user added x.txt, replaced hello.txt, and has a file where the update
        // puts a directory's ("new.txt" would be fine; "docs.txt/z" sits under the update's file).
        SimFlash f = freshFlash();
        {
                const RomImage booted = openTree(f);
                RomEditor      e(f, &booted, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "x.txt", "mine\n").isOk());
                REQUIRE(installOne(e, "hello.txt", "my hello\n").isOk());
                REQUIRE(installOne(e, "new.txt", "my new\n").isOk());
        }
        uint8_t firmware[RomImage::HashBytes];
        std::memset(firmware, 0x77, sizeof(firmware));
        RomEditor recovery(f, nullptr, bmds::Span<const uint8_t>(firmware, sizeof(firmware)));
        REQUIRE(recovery.load().isOk());
        REQUIRE(recovery.checkApply(package).isOk());
        std::vector<std::string> displaced;
        RomEditor::Report        report;
        REQUIRE(recovery.apply(
                                package, bmds::Span<const uint8_t>(firmware, sizeof(firmware)),
                                [](void *c, StringView p) {
                                        static_cast<std::vector<std::string> *>(c)->push_back(
                                                std::string(p.data(), p.size()));
                                },
                                &displaced, report)
                        .isOk());
        CHECK(displaced == std::vector<std::string>{"hello.txt", "new.txt"}); // the system wins
        CHECK(report.removed == 1);                                           // a.txt
        CHECK(report.written == 3);                                           // B.txt, hello.txt, new.txt
        CHECK(report.kept == package.count() - 3);
        const RomImage after = openTree(f, bmds::Span<const uint8_t>(firmware, sizeof(firmware)));
        CHECK(after.verify().isOk());
        CHECK(fileText(after, "B.txt") == "B, newer\n");
        CHECK(fileText(after, "new.txt") == "new\n");
        CHECK_FALSE(has(after, "a.txt"));
        CHECK(fileText(after, "x.txt") == "mine\n"); // the user's, kept
        CHECK(fileText(after, "hello.txt") == fileText(package, "hello.txt"));
        CHECK(after.version().firmware[0] == 0x77);
        // The system's files are the package's: as a manifest of it would say.
        std::vector<bmds::RomManifestEntry> manifest;
        std::vector<std::string>            paths;
        paths.reserve(package.count());
        for (uint32_t i = 0; i < package.count(); ++i) {
                const RomImage::Entry e = package.entry(i);
                paths.push_back(std::string(e.path.data(), e.path.size()));
                bmds::RomManifestEntry m{};
                m.size = uint32_t(e.data.size());
                m.flags = e.flags;
                std::memcpy(m.hash, e.hash.data(), RomImage::HashBytes);
                manifest.push_back(m);
        }
        for (size_t i = 0; i < manifest.size(); ++i) manifest[i].path = paths[i].c_str();
        const auto counts = bmds::RomManifest::compare(
                after, bmds::Span<const bmds::RomManifestEntry>(manifest.data(), manifest.size()), nullptr, nullptr);
        CHECK(counts.matches());
        CHECK(counts.user == 1);
        // Another firmware (the old, if the new didn't make it): the tree before.
        uint8_t other[RomImage::HashBytes] = {};
        other[0] = 1;
        CHECK(fileText(openTree(f, bmds::Span<const uint8_t>(other, sizeof(other))), "hello.txt") == "my hello\n");
        // Applied again: nothing to write.
        REQUIRE(recovery.apply(package, bmds::Span<const uint8_t>(firmware, sizeof(firmware)), nullptr, nullptr, report)
                        .isOk());
        CHECK(report.written == 0);
        CHECK(report.kept == package.count() + 1 - 1); // all the package's, and x.txt is kept but not counted
        CHECK(f.badWrites == 0);

        // The user's change after it is for any firmware: none passes it over (a removal made
        // under one firmware isn't undone by booting another).
        {
                const RomImage booted = openTree(f, bmds::Span<const uint8_t>(firmware, sizeof(firmware)));
                RomEditor      e(f, &booted, bmds::Span<const uint8_t>(firmware, sizeof(firmware)));
                REQUIRE(e.load().isOk());
                const StringView gone[] = {"x.txt"};
                REQUIRE(e.remove(bmds::Span<const StringView>(gone, 1), false, report).isOk());
                const RomImage now = openTree(f, bmds::Span<const uint8_t>(other, sizeof(other)));
                CHECK(now.version().sequence == report.sequence);
                CHECK(std::all_of(now.version().firmware.begin(), now.version().firmware.end(),
                                  [](uint8_t b) { return b == 0; }));
                CHECK_FALSE(has(now, "x.txt"));
        }

        // An update that can't fit beside the user's files: refused, nothing written.
        SimFlash full = freshFlash();
        {
                const RomImage booted = openTree(full);
                RomEditor      e(full, &booted, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "huge", std::string(1500 * 1024, 'h')).isOk());
        }
        SimFlash bigPkg = freshFlash();
        {
                RomEditor e(bigPkg, nullptr, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "share/big.bin", std::string(1000 * 1024, 'b'), 6, 0).isOk());
        }
        const RomImage big = openTree(bigPkg);
        RomEditor      fullRecovery(full, nullptr, {});
        REQUIRE(fullRecovery.load().isOk());
        CHECK(fullRecovery.checkApply(big) == Error::NoSpace);
        CHECK(fullRecovery.shortfall().needed >= 1000u * 1024);
        const std::vector<uint8_t> was = full.mem;
        CHECK(fullRecovery.apply(big, {}, nullptr, nullptr, report) == Error::NoSpace);
        CHECK(full.mem == was);
}

TEST_CASE("rom editor: an update keeps the installed firmware's tree whole, in case the new firmware doesn't make it") {
        // Two updates' trees: X's (B.txt changed), then Y's (B.txt changed again, c.txt added).
        auto package = [](const char *b, bool c) {
                SimFlash  p = freshFlash();
                RomEditor e(p, nullptr, {});
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "B.txt", pattern(50000, b[0]), 6, 0).isOk());
                if (c) REQUIRE(installOne(e, "c.txt", pattern(80000, 3), 6, 0).isOk());
                return p;
        };
        SimFlash       px = package("x", false), py = package("y", true);
        const RomImage x = openTree(px), y = openTree(py);
        uint8_t        idX[RomImage::HashBytes], idY[RomImage::HashBytes];
        std::memset(idX, 0xa1, sizeof(idX));
        std::memset(idY, 0xb2, sizeof(idY));
        const bmds::Span<const uint8_t> fx(idX, sizeof(idX)), fy(idY, sizeof(idY));

        SimFlash f = freshFlash();
        {
                RomEditor e(f, nullptr, {});
                REQUIRE(e.load().isOk());
                RomEditor::Report r;
                REQUIRE(e.apply(x, fx, nullptr, nullptr, r).isOk());
        }
        // X installed; its tree. The user adds a file (for any firmware).
        {
                const RomImage booted = openTree(f, fx);
                RomEditor      e(f, &booted, fx);
                REQUIRE(e.load().isOk());
                REQUIRE(installOne(e, "mine.txt", "mine\n").isOk());
        }
        // Y's update, as recovery does it: on the newest tree, X's (the installed) kept whole.
        const RomImage installed = openTree(f, fx);
        CHECK(fileText(installed, "mine.txt") == "mine\n");
        const std::string kept = bootedBytes(f, installed);
        RomEditor         e(f, &installed, {});
        REQUIRE(e.load().isOk());
        RomEditor::Report r;
        REQUIRE(e.apply(y, fy, nullptr, nullptr, r).isOk());
        CHECK(bootedBytes(f, installed) == kept);
        // Y boots its tree; X (if Y's firmware never made it) boots its own, whole.
        const RomImage withY = openTree(f, fy);
        CHECK(fileText(withY, "B.txt") == pattern(50000, 'y'));
        CHECK(fileText(withY, "c.txt") == pattern(80000, 3));
        CHECK(fileText(withY, "mine.txt") == "mine\n");
        const RomImage withX = openTree(f, fx);
        CHECK(withX.verify().isOk());
        CHECK(fileText(withX, "B.txt") == pattern(50000, 'x'));
        CHECK_FALSE(has(withX, "c.txt"));
        CHECK(f.badWrites == 0);
}

#endif // BMDS_TEST_ROM
