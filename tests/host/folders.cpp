/**
 * @file      folders.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds_files (devplan/filebrowser.md §4f, §4h, §4j): the folder model, the file operations and
 * their worker, on RAM disks; the clipboard's files; a window's history and folders.conf.
 */

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <bmds/apps.h>
#include <bmds/blockdevices.h>
#include <bmds/clipboardtype.h>
#include <bmds/copiedfiles.h>
#include <bmds/fileops.h>
#include <bmds/fileworker.h>
#include <bmds/foldermodel.h>
#include <bmds/namefilter.h>
#include <bmds/foldersettings.h>
#include <bmds/folderwatches.h>
#include <bmds/navigationhistory.h>
#include <bmds/ramblockdevice.h>
#include <bmds/vfs.h>

using bmds::Error;
using bmds::FolderModel;
using bmds::StringView;
namespace FileOps = bmds::FileOps;
namespace Vfs = bmds::Vfs;

namespace {

        const Vfs::Context Root{"/", nullptr};

        // Two RAM disks for the run (a 16 MB FAT32 and a 4 MB one), mounted while a test uses them.
        struct Disks {
                        Disks() : a(512), b(512) {
                                REQUIRE(a.allocate(64 * 2048, bmds::MemSpace::Psram).isOk());
                                REQUIRE(b.allocate(8 * 2048, bmds::MemSpace::Psram).isOk());
                        }

                        bmds::RamBlockDevice a;
                        bmds::RamBlockDevice b;
        };

        Disks &disks() {
                static Disks d;
                return d;
        }

        struct Fixture {
                        Fixture() {
                                Disks &d = disks();
                                char   name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(d.a, "disk", name).isOk());
                                a = std::string("/") + name + "/p0";
                                REQUIRE(bmds::BlockDevices::addNumbered(d.b, "disk", name).isOk());
                                b = std::string("/") + name + "/p0";
                                REQUIRE(Vfs::format(d.a, bmds::FsFormat::Fat32).isOk());
                                REQUIRE(Vfs::automount(d.a).value() == 1);
                                REQUIRE(Vfs::format(d.b, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::automount(d.b).value() == 1);
                        }

                        ~Fixture() {
                                Vfs::eject(disks().a);
                                Vfs::eject(disks().b);
                                bmds::BlockDevices::remove(disks().a);
                                bmds::BlockDevices::remove(disks().b);
                        }

                        std::string at(const char *rest) const { return a + rest; }
                        std::string onB(const char *rest) const { return b + rest; }

                        std::string a;
                        std::string b;
        };

        StringView view(const std::string &s) {
                return StringView(s.data(), s.size());
        }

        void put(const std::string &path, const std::string &text) {
                auto [h, err] = Vfs::open(view(path), Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                REQUIRE_MESSAGE(err.isOk(), path);
                REQUIRE(h->write(StringView(text.data(), text.size()), bmds::WaitForever).error().isOk());
        }

        std::string read(const std::string &path) {
                auto [h, err] = Vfs::open(view(path), Vfs::Read, Root);
                if (err.isError()) return "<" + std::string(err.desc()) + ">";
                std::string out;
                char        buf[512];
                for (;;) {
                        auto [n, rerr] = h->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                 bmds::WaitForever);
                        if (rerr.isError()) break;
                        out.append(buf, n);
                }
                return out;
        }

        bool exists(const std::string &path) {
                return Vfs::lstat(view(path), Root).isOk();
        }

        void mkdir(const std::string &path) {
                REQUIRE_MESSAGE(Vfs::mkdir(view(path), Root).isOk(), path);
        }

        std::vector<std::string> shown(const FolderModel &m) {
                std::vector<std::string> out;
                for (size_t i = 0; i < m.count(); ++i) out.emplace_back(m.name(i).data(), m.name(i).size());
                return out;
        }

        // A progress that answers taken names as told, can cancel after some bytes, and keeps what it saw.
        struct Recorder : FileOps::Progress {
                        FileOps::Resolution      answer = FileOps::Resolution::Skip;
                        uint64_t                 cancelAfter = UINT64_MAX;
                        uint64_t                 bytes = 0;
                        int                      conflicts = 0;
                        std::vector<std::string> failures;

                        bool isCancelled() override { return bytes >= cancelAfter; }
                        void advanced(uint64_t n) override { bytes += n; }
                        void failed(StringView path, Error err) override {
                                failures.push_back(std::string(path.data(), path.size()) + ": " + err.desc());
                        }
                        FileOps::Resolution conflict(StringView, StringView, bool) override {
                                ++conflicts;
                                return answer;
                        }
        };

} // namespace

TEST_CASE("FolderModel: names without case and digits as numbers") {
        CHECK(FolderModel::compareNames("File 2", "File 10") < 0);
        CHECK(FolderModel::compareNames("file 10", "File 9") > 0);
        CHECK(FolderModel::compareNames("a", "B") < 0);
        CHECK(FolderModel::compareNames("x007", "x7") == FolderModel::compareNames("x007", "x7")); // stable
        CHECK(FolderModel::compareNames("x07", "x8") < 0);
        CHECK(FolderModel::compareNames("abc", "abcd") < 0);
        CHECK(FolderModel::compareNames("README", "readme") != 0); // a fixed order still
        CHECK(FolderModel::compareNames("same", "same") == 0);
        CHECK(FolderModel::extensionOf("photo.JPG") == "JPG");
        CHECK(FolderModel::extensionOf(".profile").isEmpty());
        CHECK(FolderModel::extensionOf("Makefile").isEmpty());
}

TEST_CASE_FIXTURE(Fixture, "FolderModel: filled in batches, in order at every step, hidden ones on request") {
        const std::string dir = at("/f");
        mkdir(dir);
        for (const char *name : {"b.txt", "File 10", "File 2", "a", ".hidden", "c.dat"}) put(dir + "/" + name, name);
        put(dir + "/big.dat", std::string(5000, 'x'));
        mkdir(dir + "/Zeta");
        mkdir(dir + "/alpha");

        FolderModel m;
        REQUIRE(m.open(view(dir), Root).isOk());
        CHECK(m.path() == view(dir));
        size_t total = 0;
        while (!m.isComplete()) {
                auto [n, err] = m.fillSome(2);
                REQUIRE(err.isOk());
                total += n;
                // Sorted after every batch: folders first, then by name.
                for (size_t i = 1; i < m.count(); ++i) {
                        const bool dirs = m.entry(i - 1).isDirectory() && !m.entry(i).isDirectory();
                        REQUIRE((dirs || (m.entry(i - 1).isDirectory() == m.entry(i).isDirectory() &&
                                          FolderModel::compareNames(m.name(i - 1), m.name(i)) < 0)));
                }
        }
        CHECK(total == 9);
        CHECK(shown(m) ==
              std::vector<std::string>{"alpha", "Zeta", "a", "b.txt", "big.dat", "c.dat", "File 2", "File 10"});
        CHECK(m.shownBytes() == 1 + 5 + 5000 + 5 + 6 + 7); // not .hidden's
        CHECK(m.indexOf("File 2") == 6);
        CHECK(m.indexOf("nothing") == -1);

        // Formats by name when read (FB12); none for a name that says nothing, until set.
        CHECK(m.entry(size_t(m.indexOf("alpha"))).format == "inode/directory");
        CHECK(m.entry(size_t(m.indexOf("b.txt"))).format == "text/plain");
        CHECK(m.entry(size_t(m.indexOf("File 2"))).format.isEmpty());
        m.setFormat(size_t(m.indexOf("File 2")), "application/octet-stream");
        CHECK(m.entry(size_t(m.indexOf("File 2"))).format == "application/octet-stream");
        CHECK(m.hiddenCount() == 1);

        m.setShowHidden(true);
        CHECK(shown(m).size() == 9);
        CHECK(m.name(2) == ".hidden");
        CHECK(m.hiddenCount() == 0);
        m.setShowHidden(false);

        // A name filter leaves out files only, without case; filled later, they follow it too.
        REQUIRE(m.setNameFilter("*.DAT ; *.txt").isOk());
        CHECK(shown(m) == std::vector<std::string>{"alpha", "Zeta", "b.txt", "big.dat", "c.dat"});
        REQUIRE(m.setNameFilter(bmds::NameFilter::NoFiles).isOk());
        CHECK(shown(m) == std::vector<std::string>{"alpha", "Zeta"});
        REQUIRE(m.setNameFilter("").isOk());
        CHECK(shown(m).size() == 8);

        m.setSort(FolderModel::Sort::Size, true); // the largest first, folders still first, ties by name up
        CHECK(shown(m) ==
              std::vector<std::string>{"alpha", "Zeta", "big.dat", "File 10", "File 2", "b.txt", "c.dat", "a"});
        m.setSort(FolderModel::Sort::Name, true);
        CHECK(shown(m) ==
              std::vector<std::string>{"Zeta", "alpha", "File 10", "File 2", "c.dat", "big.dat", "b.txt", "a"});
        m.setSort(FolderModel::Sort::Type, false); // no extension, then dat, then txt
        CHECK(shown(m) ==
              std::vector<std::string>{"alpha", "Zeta", "a", "File 2", "File 10", "big.dat", "c.dat", "b.txt"});

        // Another folder: the last one's entries gone; a file isn't a folder.
        REQUIRE(m.open(view(dir + "/alpha"), Root).isOk());
        CHECK(m.fillSome(10).value() == 0);
        CHECK(m.isComplete());
        CHECK(m.count() == 0);
        CHECK(m.open(view(dir + "/a"), Root) == Error::NotDir);
        CHECK(m.open(view(dir + "/none"), Root).isError());
}

TEST_CASE("NameFilter: patterns by ;, * and ? without case, the first extension") {
        using bmds::NameFilter::firstExtension;
        using bmds::NameFilter::matches;
        using bmds::NameFilter::matchesOne;
        CHECK(matches("", "anything"));
        CHECK(matches(" ; ", "anything"));
        CHECK(matches("*.mp3;*.ogg", "Song.OGG"));
        CHECK_FALSE(matches("*.mp3;*.ogg", "song.ogg.txt"));
        CHECK(matchesOne("a?c*", "abcdef"));
        CHECK(matchesOne("*a*b*c", "xxaxxbxxbxc"));
        CHECK_FALSE(matchesOne("*a*b*c", "xxaxxbxxbx"));
        CHECK(matchesOne("*", ""));
        CHECK_FALSE(matchesOne("?", ""));
        CHECK(matchesOne("**x", "x"));
        CHECK_FALSE(matches(bmds::NameFilter::NoFiles, "a"));
        CHECK(firstExtension("*.mp3;*.ogg") == "mp3");
        CHECK(firstExtension(" *.m3u8") == "m3u8");
        CHECK(firstExtension("*").isEmpty());
        CHECK(firstExtension("*.*").isEmpty());
        CHECK(firstExtension("a*.txt").isEmpty());
        CHECK(firstExtension("*.tar.gz").isEmpty());
}

TEST_CASE_FIXTURE(Fixture, "FileOps: copies, taken names, trees, and not into itself") {
        mkdir(at("/src"));
        mkdir(at("/src/sub"));
        put(at("/src/one.txt"), "one");
        put(at("/src/sub/two.txt"), "two");
        mkdir(at("/dst"));
        Recorder     p;
        bmds::String made;

        REQUIRE(FileOps::copy(view(at("/src/one.txt")), view(at("/dst")), Root, p, &made).isOk());
        CHECK(made.view() == view(at("/dst/one.txt")));
        CHECK(read(at("/dst/one.txt")) == "one");
        CHECK(p.bytes == 3);

        // Taken: skipped by default, beside it with Keep Both, over it with Replace.
        put(at("/src/one.txt"), "ONE");
        REQUIRE(FileOps::copy(view(at("/src/one.txt")), view(at("/dst")), Root, p, &made).isOk());
        CHECK(p.conflicts == 1);
        CHECK(made.isEmpty());
        CHECK(read(at("/dst/one.txt")) == "one");
        p.answer = FileOps::Resolution::KeepBoth;
        REQUIRE(FileOps::copy(view(at("/src/one.txt")), view(at("/dst")), Root, p, &made).isOk());
        CHECK(made.view() == view(at("/dst/one (copy).txt")));
        p.answer = FileOps::Resolution::Replace;
        REQUIRE(FileOps::copy(view(at("/src/one.txt")), view(at("/dst")), Root, p).isOk());
        CHECK(read(at("/dst/one.txt")) == "ONE");
        p.answer = FileOps::Resolution::Cancel;
        CHECK(FileOps::copy(view(at("/src/one.txt")), view(at("/dst")), Root, p) == Error::Interrupted);

        // Into the folder it is in: beside it, without asking.
        const int asked = p.conflicts;
        REQUIRE(FileOps::copy(view(at("/dst/one.txt")), view(at("/dst")), Root, p, &made).isOk());
        CHECK(p.conflicts == asked);
        CHECK(made.view() == view(at("/dst/one (copy 2).txt")));

        // A tree; then onto itself, merged with Replace (the file inside asked again).
        REQUIRE(FileOps::copy(view(at("/src")), view(at("/dst")), Root, p, &made).isOk());
        CHECK(read(at("/dst/src/sub/two.txt")) == "two");
        put(at("/src/sub/three.txt"), "three");
        put(at("/src/sub/two.txt"), "TWO");
        p.answer = FileOps::Resolution::Replace;
        const int before = p.conflicts;
        REQUIRE(FileOps::copy(view(at("/src")), view(at("/dst")), Root, p).isOk());
        CHECK(p.conflicts == before + 4); // src, one.txt, sub and two.txt: three.txt is new
        CHECK(read(at("/dst/src/sub/two.txt")) == "TWO");
        CHECK(read(at("/dst/src/sub/three.txt")) == "three");

        // Not into itself.
        CHECK(FileOps::copy(view(at("/src")), view(at("/src/sub")), Root, p) == Error::Invalid);
        CHECK(FileOps::copy(view(at("/src")), view(at("/src")), Root, p) == Error::Invalid);
        CHECK(FileOps::copy(view(at("/src/one.txt")), view(at("/src/one.txt")), Root, p) == Error::Invalid);
        CHECK(FileOps::copy(view(at("/src/sub")), view(at("/src/one.txt")), Root, p) == Error::NotDir);
        CHECK(p.failures.empty());
}

TEST_CASE_FIXTURE(Fixture, "FileOps: moves rename on one filesystem and copy across, removals, cancelling") {
        mkdir(at("/m"));
        mkdir(at("/m/tree"));
        put(at("/m/tree/x.txt"), "x");
        put(at("/m/file.txt"), "file");
        mkdir(at("/to"));
        Recorder p;

        bmds::String made;
        REQUIRE(FileOps::move(view(at("/m/file.txt")), view(at("/to")), Root, p, &made).isOk());
        CHECK(!exists(at("/m/file.txt")));
        CHECK(read(at("/to/file.txt")) == "file");
        CHECK(p.bytes == 0); // a rename

        // Across filesystems: copied, then the source gone.
        REQUIRE(FileOps::move(view(at("/m/tree")), view(b), Root, p, &made).isOk());
        CHECK(made.view() == view(onB("/tree")));
        CHECK(read(onB("/tree/x.txt")) == "x");
        CHECK(!exists(at("/m/tree")));
        CHECK(p.bytes == 1);

        // Measured, then removed.
        mkdir(at("/m/r"));
        mkdir(at("/m/r/s"));
        put(at("/m/r/s/a"), "12345");
        put(at("/m/r/b"), "123");
        const std::string r = at("/m/r"), file = at("/to/file.txt");
        const StringView  paths[] = {view(r), view(file)};
        auto [totals, terr] = FileOps::measure(paths, Root, p);
        REQUIRE(terr.isOk());
        CHECK(totals.items == 5);
        CHECK(totals.bytes == 12);
        REQUIRE(FileOps::remove(view(at("/m/r")), Root, p).isOk());
        CHECK(!exists(at("/m/r")));

        // Cancelled during a large file: nothing of it left.
        put(at("/m/big"), std::string(200 * 1024, 'b'));
        Recorder c;
        c.cancelAfter = 32 * 1024;
        CHECK(FileOps::copy(view(at("/m/big")), view(b), Root, c) == Error::Interrupted);
        CHECK(!exists(onB("/big")));
        CHECK(c.bytes < 200 * 1024);
}

TEST_CASE_FIXTURE(Fixture, "FileOps: renames, new items, copy names, and the time kept") {
        mkdir(at("/n"));
        put(at("/n/old.txt"), "o");
        bmds::String made;
        REQUIRE(FileOps::rename(view(at("/n/old.txt")), "new.txt", Root, &made).isOk());
        CHECK(made.view() == view(at("/n/new.txt")));
        CHECK(read(at("/n/new.txt")) == "o");
        for (const char *bad : {"", ".", "..", "a/b"})
                CHECK(FileOps::rename(view(at("/n/new.txt")), bad, Root) == Error::Invalid);
        put(at("/n/other"), "x");
        CHECK(FileOps::rename(view(at("/n/other")), "new.txt", Root) == Error::Exists);
        // Only the case changed (FAT finds the old name under the new).
        REQUIRE(FileOps::rename(view(at("/n/new.txt")), "NEW.txt", Root).isOk());
        FolderModel m;
        REQUIRE(m.open(view(at("/n")), Root).isOk());
        (void)m.fillSome(10);
        CHECK(m.indexOf("NEW.txt") >= 0);
        CHECK(m.indexOf("new.txt") == -1);

        auto [f1, e1] = FileOps::makeNew(view(at("/n")), FileOps::NewKind::Folder, Root);
        auto [f2, e2] = FileOps::makeNew(view(at("/n")), FileOps::NewKind::Folder, Root);
        auto [t1, e3] = FileOps::makeNew(view(at("/n")), FileOps::NewKind::TextDocument, Root);
        auto [t2, e4] = FileOps::makeNew(view(at("/n")), FileOps::NewKind::TextDocument, Root);
        REQUIRE((e1.isOk() && e2.isOk() && e3.isOk() && e4.isOk()));
        CHECK(f1.view() == view(at("/n/New Folder")));
        CHECK(f2.view() == view(at("/n/New Folder (2)")));
        CHECK(t2.view() == view(at("/n/New Text Document (2).txt")));
        CHECK(Vfs::stat(f2.view(), Root).value().isDirectory());

        CHECK(FileOps::copyName(view(at("/n")), "free.txt", Root).value().view() == "free.txt");
        CHECK(FileOps::copyName(view(at("/n")), "other", Root).value().view() == "other (copy)");

        // The time kept by a copy.
        const bmds::Timestamp when = bmds::Timestamp::fromSeconds(1'600'000'000);
        REQUIRE(Vfs::setModified(view(at("/n/other")), when, Root).isOk());
        Recorder p;
        REQUIRE(FileOps::copy(view(at("/n/other")), view(b), Root, p).isOk());
        CHECK(Vfs::stat(view(onB("/other")), Root).value().modified.seconds() ==
              Vfs::stat(view(at("/n/other")), Root).value().modified.seconds());
        // The trash through a linked folder (as /home is): the file's own filesystem's trash, the
        // link left alone.
        mkdir(at("/real"));
        put(at("/real/doomed.txt"), "d");
        REQUIRE(Vfs::symlink(view(at("/real")), view(onB("/linked")), Root).isOk());
        REQUIRE(FileOps::trash(view(onB("/linked/doomed.txt")), Root).isOk());
        CHECK(!exists(at("/real/doomed.txt")));
        CHECK(read(at("/.trash/files/doomed.txt")) == "d");
        CHECK(exists(onB("/linked")));
        // The folder model shows a link as what it leads to.
        FolderModel lm;
        REQUIRE(lm.open(view(b), Root).isOk());
        while (!lm.isComplete()) (void)lm.fillSome(10);
        const int li = lm.indexOf("linked");
        REQUIRE(li >= 0);
        CHECK(lm.entry(size_t(li)).isDirectory());
        CHECK(lm.entry(size_t(li)).link);
        CHECK(li == 0); // a folder: first
        CHECK(FileOps::baseName("/a/b/") == "b");
        CHECK(FileOps::baseName("name") == "name");
}

namespace {

        // What FolderWatches told: the notifies, then each folder taken with its changes.
        struct WatchLog {
                        static void notified(void *arg) {
                                ++static_cast<WatchLog *>(arg)->notifies;
                                return;
                        }

                        static void changed(void *arg, StringView path, uint8_t changes) {
                                static_cast<WatchLog *>(arg)->told.push_back(std::string(path.data(), path.size()) +
                                                                             " " + std::to_string(changes));
                                return;
                        }

                        std::vector<std::string> take(bmds::FolderWatches &w) {
                                told.clear();
                                w.take(changed, this);
                                notifies = 0;
                                return told;
                        }

                        int                      notifies = 0;
                        std::vector<std::string> told;
        };

        constexpr int Entries = bmds::DirectoryWatch::Entries;
        constexpr int Contents = bmds::DirectoryWatch::Contents;
        constexpr int Mounts = bmds::DirectoryWatch::Mounts;

} // namespace

TEST_CASE_FIXTURE(Fixture, "FolderWatches: one watch a folder, uses counted, changes merged, mounts") {
        mkdir(at("/one"));
        mkdir(at("/two"));
        WatchLog             log;
        bmds::FolderWatches *w = nullptr;
        {
                bmds::FolderWatches watches(WatchLog::notified, &log);
                w = &watches;
                CHECK(watches.add(view(at("/one"))).isOk());
                CHECK(watches.add(view(at("/one"))).isOk());
                CHECK(watches.add(view(at("/two"))).isOk());
                CHECK(watches.add(view(at("/later"))).isOk()); // not there: watched for mounts
                CHECK(watches.count() == 3);
                CHECK(watches.uses(view(at("/one"))) == 2);
                CHECK(log.take(watches).empty());
                // Many changes, one call for each folder.
                put(at("/one/a"), "a");
                put(at("/one/b"), "b");
                mkdir(at("/one/sub"));
                mkdir(at("/two/sub"));
                CHECK(log.notifies == 4);
                CHECK(log.take(watches) ==
                      std::vector<std::string>{at("/one") + " " + std::to_string(Entries | Contents),
                                               at("/two") + " " + std::to_string(Entries)});
                CHECK(log.take(watches).empty());
                // Forgotten: read by its owner after its own change.
                mkdir(at("/one/sub2"));
                watches.forget(view(at("/one")));
                CHECK(log.take(watches).empty());
                // A use dropped keeps it; the last stops it.
                watches.remove(view(at("/one")));
                CHECK(watches.uses(view(at("/one"))) == 1);
                put(at("/one/c"), "c");
                CHECK(log.take(watches) == std::vector<std::string>{at("/one") + " " + std::to_string(Contents)});
                watches.remove(view(at("/one")));
                CHECK(watches.count() == 2);
                put(at("/one/d"), "d");
                CHECK(log.notifies == 0);
                CHECK(log.take(watches).empty());
                // A mount: every folder told, and looked for again (the one not there before is now).
                Vfs::eject(disks().a);
                REQUIRE(Vfs::automount(disks().a).value() == 1);
                mkdir(at("/later"));
                CHECK(log.take(watches) == std::vector<std::string>{at("/two") + " " + std::to_string(Mounts),
                                                                    at("/later") + " " + std::to_string(Mounts)});
                put(at("/later/x"), "x");
                CHECK(log.take(watches) == std::vector<std::string>{at("/later") + " " + std::to_string(Contents)});
                // A call may remove what it is told of.
                put(at("/two/x"), "x");
                put(at("/later/y"), "y");
                watches.take(
                        [](void *arg, StringView path, uint8_t) {
                                static_cast<bmds::FolderWatches *>(arg)->remove(path);
                                return;
                        },
                        w);
                CHECK(watches.count() == 0);
        }
        put(at("/two/after"), "x"); // the watches gone with it
        CHECK(log.notifies == 2);
}

TEST_CASE("NavigationHistory: back, forward, a visit dropping what was ahead, the oldest going") {
        bmds::NavigationHistory h;
        CHECK(h.current().isEmpty());
        CHECK_FALSE(h.canGoBack());
        CHECK(h.back().isEmpty());
        REQUIRE(h.visit("/a").isOk());
        REQUIRE(h.visit("/a").isOk()); // the current one: not again
        CHECK(h.count() == 1);
        REQUIRE(h.visit("/a/b").isOk());
        REQUIRE(h.visit("/a/b/c").isOk());
        CHECK(h.current() == "/a/b/c");
        CHECK(h.back() == "/a/b");
        CHECK(h.back() == "/a");
        CHECK_FALSE(h.canGoBack());
        CHECK(h.back().isEmpty());
        CHECK(h.current() == "/a");
        CHECK(h.forward() == "/a/b");
        CHECK(h.canGoForward());
        REQUIRE(h.visit("/x").isOk()); // /a/b/c dropped
        CHECK_FALSE(h.canGoForward());
        CHECK(h.count() == 3);
        CHECK(h.back() == "/a/b");
        CHECK(h.forward() == "/x");
        CHECK(h.forward().isEmpty());
        // The oldest goes past MaxKept.
        h.clear();
        for (size_t i = 0; i < bmds::NavigationHistory::MaxKept + 5; ++i) {
                const std::string p = "/f" + std::to_string(i);
                REQUIRE(h.visit(StringView(p.c_str())).isOk());
        }
        CHECK(h.count() == bmds::NavigationHistory::MaxKept);
        CHECK(h.current() == "/f36");
        StringView first;
        while (h.canGoBack()) first = h.back();
        CHECK(first == "/f5");
        // Parents.
        CHECK(bmds::NavigationHistory::parentOf("/a/b") == "/a");
        CHECK(bmds::NavigationHistory::parentOf("/a/b/") == "/a");
        CHECK(bmds::NavigationHistory::parentOf("/a") == "/");
        CHECK(bmds::NavigationHistory::parentOf("/") == "/");
}

TEST_CASE("FolderSettings: read, looked up, set newest, the oldest going, written back") {
        using bmds::FolderSettings;
        FolderSettings s;
        REQUIRE(s.parse("# a comment\n"
                        "[/sd0/p0/photos]\nView=Details\nSort=size\nDescending=true\nTree=200\n"
                        "[relative]\nView=details\n"                           // not a path: dropped
                        "[/home]\nView=nonsense\nSort=type\nTree=0\nOther=1\n" // a bad view, tree: defaults
                        "[/boot]\n")
                        .isOk());
        CHECK(s.count() == 3);
        FolderSettings::Values v;
        REQUIRE(s.find("/sd0/p0/photos", v));
        CHECK(v.view == FolderSettings::View::Details);
        CHECK(v.sort == FolderModel::Sort::Size);
        CHECK(v.descending);
        CHECK(v.tree == 200);
        REQUIRE(s.find("/home", v));
        CHECK(v.view == FolderSettings::View::Icons);
        CHECK(v.sort == FolderModel::Sort::Type);
        CHECK_FALSE(v.descending);
        CHECK(v.tree == 0);
        CHECK(s.find("/boot", v));
        CHECK_FALSE(s.find("relative", v));
        CHECK_FALSE(s.find("/nowhere", v));
        CHECK(s.set("relative", v) == Error::Invalid);

        // Set: the newest, last when written.
        FolderSettings::Values w;
        w.view = FolderSettings::View::Details;
        w.sort = FolderModel::Sort::Modified;
        REQUIRE(s.set("/sd0/p0/photos", w).isOk());
        CHECK(s.count() == 3);
        bmds::String text;
        REQUIRE(s.write(text).isOk());
        const std::string out(text.cStr());
        CHECK(out.find("[/home]") < out.find("[/boot]"));
        CHECK(out.find("[/boot]") < out.find("[/sd0/p0/photos]"));
        CHECK(out.find("[/sd0/p0/photos]\nView=details\nSort=modified\nDescending=false\n") != std::string::npos);
        CHECK(out.find("Tree=") == std::string::npos); // none set now
        CHECK(out.find("Other") == std::string::npos);

        // Read back: the same.
        FolderSettings again;
        REQUIRE(again.parse(text.view()).isOk());
        CHECK(again.count() == 3);
        REQUIRE(again.find("/sd0/p0/photos", v));
        CHECK(v.sort == FolderModel::Sort::Modified);
        bmds::String text2;
        REQUIRE(again.write(text2).isOk());
        CHECK(text2.view() == text.view());

        // Past MaxFolders the oldest go; one set again later stays.
        FolderSettings many;
        for (size_t i = 0; i < FolderSettings::MaxFolders + 10; ++i) {
                const std::string p = "/d" + std::to_string(i);
                REQUIRE(many.set(StringView(p.c_str()), v).isOk());
                if (i == 200) REQUIRE(many.set("/d0", v).isOk()); // newer than /d1 to /d200
        }
        CHECK(many.count() == FolderSettings::MaxFolders);
        CHECK(many.find("/d0", v));
        CHECK_FALSE(many.find("/d1", v));
        CHECK_FALSE(many.find("/d10", v));
        CHECK(many.find("/d11", v));
        CHECK(many.find("/d265", v));
}

TEST_CASE("CopiedFiles: rendered as URI lists, GNOME's and text; read back; cut ones looked up") {
        bmds::CopiedFiles files;
        CHECK(files.isEmpty());
        REQUIRE(files.set("/a/b c%.txt\n/x", false).isOk());
        bmds::String out;
        REQUIRE(files.render(bmds::CopiedFiles::UriList, out).isOk());
        CHECK(out.view() == "file:///a/b%20c%25.txt\r\nfile:///x\r\n");
        REQUIRE(files.render(bmds::CopiedFiles::GnomeFiles, out).isOk());
        CHECK(out.view() == "copy\nfile:///a/b%20c%25.txt\nfile:///x");
        REQUIRE(files.render(bmds::ClipboardType::Text, out).isOk());
        CHECK(out.view() == "/a/b c%.txt\n/x");
        CHECK(!files.isCut());
        CHECK(!files.isCut("/x"));

        // Cut: each path found, nothing else (a prefix, a folder of one).
        REQUIRE(files.set("/m/zeta\n/m/alpha\n/m/mid\n", true).isOk());
        REQUIRE(files.render(bmds::CopiedFiles::GnomeFiles, out).isOk());
        CHECK(out.view() == "cut\nfile:///m/zeta\nfile:///m/alpha\nfile:///m/mid");
        CHECK(files.isCut("/m/alpha"));
        CHECK(files.isCut("/m/mid"));
        CHECK(files.isCut("/m/zeta"));
        CHECK(!files.isCut("/m/alph"));
        CHECK(!files.isCut("/m"));
        CHECK(!files.isCut("/m/zeta/x"));
        // By folder and name, as a view asks.
        CHECK(files.isCut("/m", "alpha"));
        CHECK(files.isCut("/m/", "mid"));
        CHECK(files.isCut("/m", "zeta"));
        CHECK(!files.isCut("/m", "alph"));
        CHECK(!files.isCut("/m", "alphab"));
        CHECK(!files.isCut("/", "m"));
        CHECK(!files.isCut("/m/zeta", "x"));
        CHECK(!files.isCut("/n", "mid"));
        files.clear();
        CHECK(!files.isCut("/m/mid"));

        // Read back: GNOME's (a host, a trailing line), a URI list (comments, other schemes).
        bool         cut = false;
        bmds::String paths;
        REQUIRE(bmds::CopiedFiles::parse(bmds::CopiedFiles::GnomeFiles, "cut\nfile:///a/b%20c\nfile://host/x\n", cut,
                                         paths)
                        .isOk());
        CHECK(cut);
        CHECK(paths.view() == "/a/b c\n/x");
        REQUIRE(bmds::CopiedFiles::parse(bmds::CopiedFiles::GnomeFiles, "copy\nfile:///y", cut, paths).isOk());
        CHECK(!cut);
        CHECK(paths.view() == "/y");
        REQUIRE(bmds::CopiedFiles::parse(bmds::CopiedFiles::UriList,
                                         "# a comment\r\nhttp://example.com/\r\nfile:///one\r\nfile:///two%2F\r\n", cut,
                                         paths)
                        .isOk());
        CHECK(!cut);
        CHECK(paths.view() == "/one\n/two/");
        CHECK(bmds::CopiedFiles::parse(bmds::CopiedFiles::UriList, "# nothing\r\n", cut, paths) == Error::Empty);
}

namespace {

        // A worker's news, waited for on the test's thread.
        struct WorkerLog {
                        std::mutex              lock;
                        std::condition_variable changed;
                        int                     notifies = 0;

                        static void notified(void *arg) {
                                auto *self = static_cast<WorkerLog *>(arg);
                                {
                                        std::lock_guard<std::mutex> l(self->lock);
                                        ++self->notifies;
                                }
                                self->changed.notify_all();
                                return;
                        }

                        // Waits (5 s at most) until @p ready says so, asked after each notify.
                        template <typename F> bool waitFor(F ready) {
                                std::unique_lock<std::mutex> l(lock);
                                for (;;) {
                                        l.unlock();
                                        if (ready()) return true;
                                        l.lock();
                                        const int seen = notifies;
                                        if (!changed.wait_for(l, std::chrono::seconds(5),
                                                              [&] { return notifies != seen; }))
                                                return false;
                                }
                        }
        };

        bmds::FileWorker::Done waitDone(WorkerLog &log, bmds::FileWorker &worker) {
                bmds::FileWorker::Done d;
                REQUIRE(log.waitFor([&] { return worker.takeDone(d); }));
                return d;
        }

        bmds::FileWorker::Status waitAsking(WorkerLog &log, bmds::FileWorker &worker) {
                bmds::FileWorker::Status s;
                REQUIRE(log.waitFor([&] {
                        worker.status(s);
                        return s.asking;
                }));
                return s;
        }

        size_t entries(const std::string &path) {
                size_t n = 0;
                auto [dir, err] = Vfs::openDirectory(view(path), Root);
                while (err.isOk() && dir.next().isOk()) ++n;
                return n;
        }

} // namespace

TEST_CASE_FIXTURE(Fixture, "FileWorker: jobs in order, questions answered for all, cancels, failures") {
        using Kind = bmds::FileWorker::Kind;
        using Resolution = FileOps::Resolution;
        mkdir(at("/tree"));
        put(at("/tree/a.txt"), "a");
        put(at("/tree/b.txt"), "bb");
        put(at("/tree/c.txt"), "ccc");
        WorkerLog        log;
        bmds::FileWorker worker(WorkerLog::notified, &log);
        CHECK(!worker.isBusy());
        CHECK(worker.add(Kind::Copy, " \n", view(b)).error() == Error::Invalid);

        // A tree copied across.
        auto [first, ferr] = worker.add(Kind::Copy, view(at("/tree")), view(b));
        REQUIRE(ferr.isOk());
        bmds::FileWorker::Done d = waitDone(log, worker);
        CHECK(d.job == first);
        CHECK(d.kind == Kind::Copy);
        CHECK(d.err.isOk());
        CHECK(d.failures == 0);
        CHECK(read(onB("/tree/c.txt")) == "ccc");

        // Again: the folder asks (merged), its first file asks, kept both for all the others.
        auto [again, aerr] = worker.add(Kind::Copy, view(at("/tree")), view(b));
        REQUIRE(aerr.isOk());
        bmds::FileWorker::Status s = waitAsking(log, worker);
        CHECK(s.job == again);
        CHECK(s.askIsDirectory);
        CHECK(s.askTarget.view() == view(onB("/tree")));
        CHECK(s.askSource.view() == view(at("/tree")));
        CHECK(s.totalItems == 4);
        CHECK(s.totalBytes == 6);
        worker.answer(Resolution::Replace, false);
        s = waitAsking(log, worker);
        CHECK(!s.askIsDirectory);
        worker.answer(Resolution::KeepBoth, true);
        d = waitDone(log, worker);
        CHECK(d.job == again);
        CHECK(d.err.isOk());
        CHECK(entries(onB("/tree")) == 6);
        CHECK(read(onB("/tree/b (copy).txt")) == "bb");

        // Queued behind a question: dropped by its number; the one asking cancelled.
        auto [asks, qerr] = worker.add(Kind::Copy, view(at("/tree/a.txt")), view(onB("/tree")));
        REQUIRE(qerr.isOk());
        s = waitAsking(log, worker);
        auto [queued, uerr] = worker.add(Kind::Remove, view(onB("/tree")), StringView());
        REQUIRE(uerr.isOk());
        worker.status(s);
        CHECK(s.waiting == 1);
        worker.cancel(queued);
        worker.status(s);
        CHECK(s.waiting == 0);
        worker.cancel(asks);
        d = waitDone(log, worker);
        CHECK(d.job == asks);
        CHECK(d.err == Error::Interrupted);
        CHECK(exists(onB("/tree"))); // the removal never ran

        // A move across filesystems, one within, and a path not there: the others done.
        mkdir(at("/to"));
        put(at("/one.txt"), "1");
        const std::string moves = at("/tree") + "\n" + at("/one.txt") + "\n" + at("/missing");
        auto [moved, merr] = worker.add(Kind::Move, view(moves), view(at("/to")));
        REQUIRE(merr.isOk());
        d = waitDone(log, worker);
        CHECK(d.job == moved);
        CHECK(d.err == Error::NotExist);
        CHECK(d.failures == 1);
        CHECK(d.failed.view() == view(at("/missing")));
        CHECK(read(at("/to/one.txt")) == "1");
        CHECK(read(at("/to/tree/a.txt")) == "a");
        CHECK(!exists(at("/tree")));
        auto [across, xerr] = worker.add(Kind::Move, view(at("/to/tree")), view(b));
        REQUIRE(xerr.isOk());
        s = waitAsking(log, worker); // b's tree is there
        worker.answer(Resolution::Replace, true);
        d = waitDone(log, worker);
        CHECK(d.err.isOk());
        CHECK(!exists(at("/to/tree")));

        // Removed, then nothing left running.
        auto [removed, rerr] = worker.add(Kind::Remove, view(onB("/tree")), StringView());
        REQUIRE(rerr.isOk());
        d = waitDone(log, worker);
        CHECK(d.job == removed);
        CHECK(d.err.isOk());
        CHECK(!exists(onB("/tree")));
        CHECK(log.waitFor([&] { return !worker.isBusy(); }));
        worker.reap();
}

namespace {

        // An application folder: its app.desktop (@p extra lines added) and a program.
        void makeApp(const std::string &folder, const std::string &extra, const char *program = "prog") {
                mkdir(folder);
                put(folder + "/app.desktop", "[Desktop Entry]\nName=App\nExec=" + std::string(program) + "\n" + extra);
                const std::string prog(program);
                if (prog.find('/') != std::string::npos) mkdir(folder + "/" + prog.substr(0, prog.find('/')));
                if (!prog.empty()) put(folder + "/" + prog, "program");
        }

        // An Apps run, its roots fixed, its scans waited for.
        struct AppsRun {
                        std::string             roots;
                        std::mutex              lock;
                        std::condition_variable changed;
                        bmds::Apps              apps;

                        static bmds::Error rootsOf(void *arg, bmds::String &out) {
                                return out.assign(view(static_cast<AppsRun *>(arg)->roots));
                        }

                        static void notify(void *arg) {
                                auto                       *self = static_cast<AppsRun *>(arg);
                                std::lock_guard<std::mutex> g(self->lock);
                                self->changed.notify_all();
                                return;
                        }

                        void start() {
                                bmds::Apps::Config c;
                                c.roots = rootsOf;
                                c.rootsArg = this;
                                c.notify = notify;
                                c.notifyArg = this;
                                c.links = true;
                                REQUIRE(apps.start(c).isOk());
                        }

                        // Waits up to @p ms for /apps to list @p names (scans may come between).
                        bool waitFor(const std::vector<std::string> &names, int ms = 10000);

                        std::vector<std::string> commands() {
                                std::vector<std::string> out;
                                apps.visit(
                                        [](void *arg, const bmds::Apps::App &a) {
                                                static_cast<std::vector<std::string> *>(arg)->push_back(
                                                        std::string(a.command) + "=" + a.id + ":" + a.program.cStr());
                                        },
                                        &out);
                                return out;
                        }

                        std::vector<std::string> problems() {
                                std::vector<std::string> out;
                                apps.visitProblems(
                                        [](void *arg, const bmds::Apps::Problem &p) {
                                                static_cast<std::vector<std::string> *>(arg)->push_back(
                                                        std::string(p.folder.cStr()) + ": " + p.why);
                                        },
                                        &out);
                                return out;
                        }
        };

        std::vector<std::string> listed(const char *path) {
                auto [dir, err] = Vfs::openDirectory(path, Root);
                REQUIRE(err.isOk());
                std::vector<std::string> names;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr.isError()) break;
                        names.emplace_back(info.name.cStr());
                }
                std::sort(names.begin(), names.end());
                return names;
        }

        bool AppsRun::waitFor(const std::vector<std::string> &names, int ms) {
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                for (;;) {
                        if (listed("/apps") == names) return true;
                        std::unique_lock<std::mutex> g(lock);
                        if (changed.wait_until(g, end) == std::cv_status::timeout) return listed("/apps") == names;
                }
        }

} // namespace

TEST_CASE("Apps: names") {
        using bmds::Apps;
        CHECK(Apps::isValidName("blocks"));
        CHECK(Apps::isValidName("Doom-2_x.y+z"));
        CHECK(!Apps::isValidName(""));
        CHECK(!Apps::isValidName(".hidden"));
        CHECK(!Apps::isValidName("-flag"));
        CHECK(!Apps::isValidName("a b"));
        CHECK(!Apps::isValidName("a/b"));
        CHECK(!Apps::isValidName(std::string(32, 'a').c_str()));
}

TEST_CASE_FIXTURE(Fixture, "Apps: folders found, ids and commands, the first winning, problems, /apps") {
        mkdir(at("/apps"));
        makeApp(at("/apps/blocks.app"), "X-BMDS-Display=FullScreen\n");
        makeApp(at("/apps/Tour.APP"), "X-BMDS-Id=tour\nX-BMDS-Command=walk\n", "bin/tour");
        mkdir(at("/apps/broken.app"));
        makeApp(at("/apps/noprog.app"), "", "");
        put(at("/apps/noprog.app/app.desktop"), "[Desktop Entry]\nName=x\nExec=missing\n");
        makeApp(at("/apps/same.app"), "X-BMDS-Command=blocks\n");
        makeApp(at("/apps/hidden.app"), "Hidden=true\n");
        makeApp(at("/apps/escape.app"), "", "");
        put(at("/apps/escape.app/app.desktop"), "[Desktop Entry]\nName=x\nExec=../blocks.app/prog\n");
        put(at("/apps/notes.txt"), "not an application");
        mkdir(onB("/apps"));
        makeApp(onB("/apps/blocks.app"), ""); // the first root's wins
        AppsRun run;
        run.roots = at("/apps") + ";" + onB("/apps") + ";/nowhere/apps";
        run.start();
        CHECK(run.commands() == std::vector<std::string>{"blocks=blocks:" + at("/apps/blocks.app/prog"),
                                                         "walk=tour:" + at("/apps/Tour.APP/bin/tour")});
        CHECK(run.problems() ==
              std::vector<std::string>{at("/apps/broken.app") + ": no app.desktop",
                                       at("/apps/escape.app") + ": Exec is not a program in the folder",
                                       at("/apps/noprog.app") + ": its program is missing",
                                       at("/apps/same.app") + ": its command is another application's",
                                       onB("/apps/blocks.app") + ": its id is another application's"});
        CHECK(run.apps.generation() == 1);
        // /apps: a link per command.
        CHECK(listed("/apps") == std::vector<std::string>{"blocks", "walk"});
        CHECK(Vfs::readLink("/apps/walk", Root).value().view() == view(at("/apps/Tour.APP/bin/tour")));
        CHECK(read("/apps/blocks") == "program");
        // A folder copied in (made, then filled): found once the changes settle.
        makeApp(at("/apps/new.app"), "");
        CHECK(run.waitFor({"blocks", "new", "walk"}));
        // An app.desktop changed: its command follows.
        put(at("/apps/new.app/app.desktop"), "[Desktop Entry]\nName=New\nExec=prog\nX-BMDS-Command=newer\n");
        CHECK(run.waitFor({"blocks", "newer", "walk"}));
        // Removed: gone.
        REQUIRE(Vfs::remove(view(at("/apps/new.app/prog")), Root).isOk());
        REQUIRE(Vfs::remove(view(at("/apps/new.app/app.desktop")), Root).isOk());
        REQUIRE(Vfs::rmdir(view(at("/apps/new.app")), Root).isOk());
        CHECK(run.waitFor({"blocks", "walk"}));
        // A root made after the start: its parent is watched meanwhile.
        const uint32_t seen = run.apps.generation();
        run.roots += ";" + at("/later");
        run.apps.rescan();
        CHECK(!run.waitFor({"blocks", "late", "walk"}, 1000));
        CHECK(run.apps.generation() == seen); // nothing changed: nothing told
        mkdir(at("/later"));
        makeApp(at("/later/late.app"), "");
        CHECK(run.waitFor({"blocks", "late", "walk"}));
        // A root removed, then made again and filled later: its new folder is watched.
        REQUIRE(Vfs::remove(view(at("/later/late.app/prog")), Root).isOk());
        REQUIRE(Vfs::remove(view(at("/later/late.app/app.desktop")), Root).isOk());
        REQUIRE(Vfs::rmdir(view(at("/later/late.app")), Root).isOk());
        REQUIRE(Vfs::rmdir(view(at("/later")), Root).isOk());
        CHECK(run.waitFor({"blocks", "walk"}));
        mkdir(at("/later"));
        CHECK(!run.waitFor({"again", "blocks", "walk"}, 1000)); // the scan of the empty root done
        makeApp(at("/later/again.app"), "");
        CHECK(run.waitFor({"again", "blocks", "walk"}));
        run.apps.stop();
        REQUIRE(Vfs::setAppLinks(bmds::Span<const Vfs::AppLink>()).isOk());
}
