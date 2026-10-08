/**
 * @file      folders.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds_files (devplan/filebrowser.md §4f, §4h): the folder model and the file operations, on RAM
 * disks; a window's history and folders.conf.
 */

#include <doctest.h>

#include <string>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/fileops.h>
#include <bmds/foldermodel.h>
#include <bmds/foldersettings.h>
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
