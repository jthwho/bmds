/**
 * @file      files.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The file commands, through the shell, on RAM disks.
 */

#include <doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <bmds/blockdevices.h>
#include <bmds/clock.h>
#include <bmds/filecommands.h>
#include <bmds/filenames.h>
#include <bmds/fscommands.h>
#include <bmds/langbuiltins.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include <bmds/trash.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"

using bmds::Error;
using bmds::Shell;
namespace Vfs = bmds::Vfs;

namespace {

        constexpr bmds::Command Commands[] = {
                {".", "file [arg]...", "", bmds::LangBuiltins::source},
                {"[", "expr ]", "", bmds::LangBuiltins::test},
                {"cat", "[file]...", "", bmds::TextCommands::cat},
                {"cd", "[dir | -]", "", bmds::FileCommands::cd},
                {"cp", "[-r] source... target", "", bmds::FileCommands::cp},
                {"df", "[-h]", "", bmds::FsCommands::df},
                {"echo", "[-neE] [word]...", "", bmds::TextCommands::echo},
                {"grep", "[-civnqF] pattern [file]...", "", bmds::TextCommands::grep},
                {"head", "[-n count] [file]...", "", bmds::TextCommands::head},
                {"hexdump", "[-C] [file]", "", bmds::FileCommands::hexdump},
                {"ln", "-s [-f] target name | -s [-f] target... dir", "", bmds::FileCommands::ln},
                {"ls", "[-la1] [path]...", "", bmds::FileCommands::ls},
                {"mkdir", "[-p] dir...", "", bmds::FileCommands::mkdir},
                {"mkfs", "[-t auto|fat32|exfat] -y DEVICE", "", bmds::FsCommands::mkfs},
                {"mount", "[DEVICE PATH]", "", bmds::FsCommands::mount},
                {"mv", "source... target", "", bmds::FileCommands::mv},
                {"pwd", "", "", bmds::FileCommands::pwd},
                {"return", "[n]", "", bmds::LangBuiltins::returnCmd},
                {"rm", "[-rf] path...", "", bmds::FileCommands::rm},
                {"rmdir", "dir...", "", bmds::FileCommands::rmdir},
                {"set", "[-+Ceux] [-+o name] [--] [arg]...", "", bmds::LangBuiltins::set},
                {"sh", "[-c command [name] | file] [arg]...", "", bmds::LangBuiltins::sh},
                {"source", "file [arg]...", "", bmds::LangBuiltins::source},
                {"test", "expr", "", bmds::LangBuiltins::test},
                {"touch", "file...", "", bmds::FileCommands::touch},
                {"trash", "path... | -l | -r name... | -e [mount]", "", bmds::FileCommands::trash},
                {"wc", "[-lwc] [file]...", "", bmds::TextCommands::wc},
        };

        // Two RAM disks, kept for the whole run (the registry and mounts are
        // global): ram-disk "a" (64 MB, FAT32 at its default path) and "b"
        // (8 MB) for moving across filesystems and for mkfs.
        struct Disks {
                        Disks() : a(512), b(512) {
                                REQUIRE(a.allocate(64 * 2048, bmds::MemSpace::Psram).isOk());
                                REQUIRE(b.allocate(8 * 2048, bmds::MemSpace::Psram).isOk());
                        }

                        bmds::RamBlockDevice a;
                        bmds::RamBlockDevice b;
                        char                 aName[bmds::BlockDevices::MaxName + 1];
                        char                 bName[bmds::BlockDevices::MaxName + 1];
        };

        Disks &disks() {
                static Disks d;
                return d;
        }

        void put(const char *path, const std::string &text) {
                auto [h, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context{"/", nullptr});
                REQUIRE_MESSAGE(err.isOk(), path);
                REQUIRE(h->write(bmds::StringView(text.data(), text.size()), bmds::WaitForever).error().isOk());
        }

        struct Fixture {
                        Fixture() {
                                Disks &d = disks();
                                REQUIRE(bmds::BlockDevices::addNumbered(d.a, "disk", d.aName).isOk());
                                REQUIRE(bmds::BlockDevices::addNumbered(d.b, "disk", d.bName).isOk());
                                REQUIRE(std::string(d.aName) == "disk0");
                                REQUIRE(Vfs::format(d.a, bmds::FsFormat::Fat32).isOk());
                                REQUIRE(Vfs::automount(d.a).value() == 1);
                                REQUIRE(Vfs::format(d.b, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::automount(d.b).value() == 1);
                                REQUIRE(bmds::Clock::setTimeZone("UTC0").isOk());
                                shell = std::make_unique<Shell>(con, bmds::Span<const bmds::Command>(Commands),
                                                                1 << 20);
                                REQUIRE(shell->init().isOk());
                                REQUIRE(shell->execute("NO_COLOR=1; COLUMNS=40; HOME=/disk0/p0") == 0);
                                con.take();
                        }

                        // The registry and mounts are shared with the other tests: leave none behind.
                        ~Fixture() {
                                shell.reset();
                                Vfs::eject(disks().a);
                                Vfs::eject(disks().b);
                                bmds::BlockDevices::remove(disks().a);
                                bmds::BlockDevices::remove(disks().b);
                        }

                        std::string out(const char *line, int expectStatus = Shell::ExitSuccess) {
                                CHECK_MESSAGE(shell->execute(line) == expectStatus, line);
                                return con.take();
                        }

                        // Types @p keys at a fresh prompt; returns what the editor echoed after it.
                        std::string type(const char *keys) {
                                shell->feed('\x03'); // cancel any line
                                shell->start();
                                (void)con.take();
                                for (const char *k = keys; *k != '\0'; ++k) shell->feed(*k);
                                return con.take();
                        }

                        FakeConsole            con;
                        std::unique_ptr<Shell> shell;
        };

} // namespace

TEST_CASE("Files: cd and pwd") {
        Fixture f;
        CHECK(f.out("pwd") == "/\n");
        CHECK(f.out("cd /disk0/p0; pwd") == "/disk0/p0\n");
        CHECK(f.out("mkdir docs; cd docs; pwd; echo $PWD $OLDPWD") == "/disk0/p0/docs\n/disk0/p0/docs /disk0/p0\n");
        CHECK(f.out("cd ..; pwd") == "/disk0/p0\n");
        CHECK(f.out("cd -") == "/disk0/p0/docs\n");
        CHECK(f.out("cd /; cd; pwd") == "/disk0/p0\n");
        CHECK(f.out("cd ../..; pwd") == "/\n");
        CHECK(f.out("cd /nowhere", Shell::ExitFailure) == "cd: /nowhere: No such file or directory\n");
        put("/disk0/p0/file", "x");
        CHECK(f.out("cd /disk0/p0/file", Shell::ExitFailure) == "cd: /disk0/p0/file: Not a directory\n");
        CHECK(f.out("cd a b", Shell::ExitFailure) == "cd: too many arguments\n");
        CHECK(f.out("pwd") == "/\n");
        // A subshell has its own directory.
        CHECK(f.out("(cd /disk0/p0; pwd); pwd") == "/disk0/p0\n/\n");
        CHECK(f.out("x=$(cd /disk0; pwd); echo $x; pwd") == "/disk0\n/\n");
}

TEST_CASE("Files: ls") {
        Fixture f;
        put("/disk0/p0/b.txt", "bee");
        put("/disk0/p0/A.txt", "ay");
        put("/disk0/p0/.hidden", "");
        CHECK(f.out("mkdir /disk0/p0/docs") == "");
        const int64_t when = bmds::Calendar::daysFromCivil(2026, 3, 4) * 86400 + 15 * 3600 + 6 * 60;
        REQUIRE(Vfs::setModified("/disk0/p0/b.txt", bmds::Timestamp::fromSeconds(when), Vfs::Context{"/", nullptr})
                        .isOk());

        CHECK(f.out("ls /disk0/p0") == "A.txt  b.txt  docs/\n");
        CHECK(f.out("ls -1 /disk0/p0") == "A.txt\nb.txt\ndocs/\n");
        CHECK(f.out("ls -a1 /disk0/p0") == ".hidden\nA.txt\nb.txt\ndocs/\n");
        CHECK(f.out("cd /disk0/p0; ls b.txt docs") == "b.txt\n\ndocs:\n");
        CHECK(f.out("ls /") == "bin/  dev/  disk0/  disk1/  etc/  share/\n");
        CHECK(f.out("ls /dev") ==
              "clip0    disk0    disk1p0  urandom\nclip0p   disk0p0  null     zero\nconsole  disk1    random\n");
        const std::string longList = f.out("ls -l /disk0/p0/b.txt /disk0/p0/A.txt");
        CHECK(longList.find("-rw-  2  ") == 0);
        CHECK(longList.find("/disk0/p0/A.txt\n") != std::string::npos);
        CHECK(longList.find("-rw-  3  2026-03-04 15:06  /disk0/p0/b.txt\n") != std::string::npos);
        CHECK(f.out("TZ=CET-1; ls -l /disk0/p0/b.txt") == "-rw-  3  2026-03-04 16:06  /disk0/p0/b.txt\n");
        CHECK(f.out("ls -l /disk0/p0").find("drw-  0  ") != std::string::npos);
        CHECK(f.out("ls /nope", Shell::ExitUsage) == "ls: /nope: No such file or directory\n");
        CHECK(f.out("ls -z", Shell::ExitUsage) == "usage: ls [-la1] [path]...\n");

        SUBCASE("columns fill top to bottom") {
                for (int i = 0; i < 7; ++i) put(("/disk0/p0/docs/file" + std::to_string(i)).c_str(), "");
                CHECK(f.out("ls /disk0/p0/docs") == "file0  file2  file4  file6\nfile1  file3  file5\n");
                CHECK(f.out("COLUMNS=12; ls /disk0/p0/docs") == "file0  file4\nfile1  file5\nfile2  file6\nfile3\n");
        }
        SUBCASE("colors when allowed") {
                CHECK(f.out("NO_COLOR=; ls /disk0/p0") == "A.txt  b.txt  \x1b[1;34mdocs/\x1b[0m\n");
        }
}

TEST_CASE("Files: the text commands read files") {
        Fixture f;
        put("/disk0/p0/one", "alpha\nbeta\ngamma\n");
        put("/disk0/p0/two", "delta\nalpha again\n");
        CHECK(f.out("cd /disk0/p0; cat one two") == "alpha\nbeta\ngamma\ndelta\nalpha again\n");
        CHECK(f.out("cat /disk0/p0/missing /disk0/p0/one", Shell::ExitFailure) ==
              "cat: /disk0/p0/missing: No such file or directory\nalpha\nbeta\ngamma\n");
        CHECK(f.out("cd /disk0/p0; head -n 1 one two") == "==> one <==\nalpha\n\n==> two <==\ndelta\n");
        CHECK(f.out("cd /disk0/p0; head -n 2 one") == "alpha\nbeta\n");
        CHECK(f.out("cd /disk0/p0; wc one two") == " 3  3 17 one\n 2  3 18 two\n 5  6 35 total\n");
        CHECK(f.out("cd /disk0/p0; wc -l one") == "3 one\n");
        CHECK(f.out("cd /disk0/p0; grep -n alpha one two") == "one:1:alpha\ntwo:2:alpha again\n");
        CHECK(f.out("cd /disk0/p0; grep -c a one two") == "one:3\ntwo:2\n");
        CHECK(f.out("cd /disk0/p0; grep beta one") == "beta\n");
        CHECK(f.out("cd /disk0/p0; grep zzz one", Shell::ExitFailure) == "");
        CHECK(f.out("cd /disk0/p0; grep a nofile", Shell::ExitUsage) == "grep: nofile: No such file or directory\n");
        CHECK(f.out("cd /disk0/p0; echo piped | cat - one") == "piped\nalpha\nbeta\ngamma\n");
        CHECK(f.out("cat /dev/null") == "");
        CHECK(f.out("cat /disk0", Shell::ExitFailure) == "cat: /disk0: Is a directory\n");
}

TEST_CASE("Files: mkdir, rmdir, rm, touch") {
        Fixture f;
        CHECK(f.out("cd /disk0/p0; mkdir -p a/b/c; mkdir -p a/b; ls -1 a/b") == "c/\n");
        CHECK(f.out("cd /disk0/p0; mkdir a", Shell::ExitFailure) == "mkdir: a: File exists\n");
        CHECK(f.out("mkdir /top", Shell::ExitFailure) == "mkdir: /top: Permission denied\n");
        CHECK(f.out("cd /disk0/p0; rmdir a", Shell::ExitFailure) == "rmdir: a: Directory not empty\n");
        CHECK(f.out("cd /disk0/p0; touch a/file new; ls -1") == "a/\nnew\n");
        CHECK(f.out("cd /disk0/p0; rm a", Shell::ExitFailure) == "rm: a: Is a directory\n");
        CHECK(f.out("cd /disk0/p0; rm -r a; rm new; ls -1") == "");
        CHECK(f.out("cd /disk0/p0; rm gone", Shell::ExitFailure) == "rm: gone: No such file or directory\n");
        CHECK(f.out("cd /disk0/p0; rm -f gone") == "");
        CHECK(f.out("rm -r /disk0/p0", Shell::ExitFailure) == "rm: /disk0/p0: Device or resource busy\n");

        // touch sets the time of an existing file to now.
        const int64_t when = bmds::Calendar::daysFromCivil(2026, 5, 6) * 86400 + 7 * 3600 + 8 * 60;
        REQUIRE(bmds::Clock::set(bmds::Timestamp::fromSeconds(when), bmds::ClockSource::Manual).isOk());
        put("/disk0/p0/old", "x");
        REQUIRE(Vfs::setModified("/disk0/p0/old", bmds::Timestamp::fromSeconds(0), Vfs::Context{"/", nullptr}).isOk());
        CHECK(f.out("touch /disk0/p0/old; ls -l /disk0/p0/old") == "-rw-  1  2026-05-06 07:08  /disk0/p0/old\n");
}

TEST_CASE("Files: cp and mv") {
        Fixture f;
        put("/disk0/p0/a", "data a");
        CHECK(f.out("cd /disk0/p0; mkdir d; cp a b; cp a d; cat b d/a") == "data adata a");
        CHECK(f.out("cd /disk0/p0; cp a a", Shell::ExitFailure) ==
              "cp: /disk0/p0/a and /disk0/p0/a are the same file\n");
        CHECK(f.out("cd /disk0/p0; cp d e", Shell::ExitFailure) == "cp: d: Is a directory (not copied without -r)\n");
        CHECK(f.out("cd /disk0/p0; mkdir d/sub; cp -r d e; ls -1 e") == "a\nsub/\n");
        CHECK(f.out("cd /disk0/p0; cp -r d d/sub", Shell::ExitFailure) == "cp: cannot copy /disk0/p0/d into itself\n");
        CHECK(f.out("cd /disk0/p0; cp a b nothere", Shell::ExitFailure) == "cp: nothere: Not a directory\n");

        // A bigger file than the copy buffer.
        std::string big;
        for (int i = 0; i < 100000; ++i) big += static_cast<char>('A' + i % 23);
        put("/disk0/p0/big", big);
        CHECK(f.out("cd /disk0/p0; cp big big2; wc -c big2") == "100000 big2\n");

        CHECK(f.out("cd /disk0/p0; mv b c; ls -1") == "a\nbig\nbig2\nc\nd/\ne/\n");
        CHECK(f.out("cd /disk0/p0; mv c a; cat a; ls -1") == "data aa\nbig\nbig2\nd/\ne/\n");
        CHECK(f.out("cd /disk0/p0; mv e d; ls -1 d") == "a\ne/\nsub/\n");
        // Across filesystems: copied, then removed.
        CHECK(f.out("cd /disk0/p0; mv d /disk1/p0; ls -1; ls -1 /disk1/p0/d") == "a\nbig\nbig2\na\ne/\nsub/\n");
        CHECK(f.out("mv /disk1/p0/d /disk1/p0/d/e", Shell::ExitFailure) == "mv: cannot copy /disk1/p0/d into itself\n");
}

TEST_CASE("Files: hexdump") {
        Fixture f;
        put("/disk0/p0/h", "Hello\n0123456789abcdefXYZ");
        CHECK(f.out("hexdump -C /disk0/p0/h") ==
              "00000000  48 65 6c 6c 6f 0a 30 31  32 33 34 35 36 37 38 39  |Hello.0123456789|\n"
              "00000010  61 62 63 64 65 66 58 59  5a                       |abcdefXYZ|\n"
              "00000019\n");
        put("/disk0/p0/z", std::string(64, '\0'));
        CHECK(f.out("hexdump /disk0/p0/z") ==
              "00000000  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|\n*\n00000040\n");
        CHECK(f.out("echo hi | hexdump -C") ==
              "00000000  68 69 0a                                          |hi.|\n00000003\n");
        CHECK(f.out("hexdump /dev/null") == "");
}

TEST_CASE("Files: df and mkfs") {
        Fixture           f;
        const std::string df = f.out("df");
        CHECK(df.find("Filesystem     1K-blocks ") == 0);
        CHECK(df.find("\n/dev/disk0p0   ") != std::string::npos);
        CHECK(df.find(" /disk0/p0\n") != std::string::npos);
        CHECK(df.find(" /disk1/p0\n") != std::string::npos);
        const std::string dfh = f.out("df -h");
        CHECK(dfh.find("Filesystem     Size Used Avail Use% Mounted on\n") == 0);
        CHECK(dfh.find("M ") != std::string::npos);

        CHECK(f.out("mkfs /dev/disk1", Shell::ExitFailure) ==
              "mkfs: this erases everything on /dev/disk1; run again with -y to go ahead\n");
        CHECK(f.out("mkfs -y /dev/nothing", Shell::ExitFailure) == "mkfs: /dev/nothing: not a block device\n");
        put("/disk1/p0/doomed", "x");
        CHECK(f.out("mkfs -t exfat -y /dev/disk1") == "/dev/disk1p0 on /disk1/p0 type exfat\n");
        CHECK(f.out("ls /disk1/p0") == "");
        CHECK(f.out("mkfs -t fat32 -y /dev/disk1p0", Shell::ExitFailure).find("mkfs: /dev/disk1p0: Invalid argument") ==
              0);
        CHECK(f.out("mkfs -y /dev/disk1p0") == "/dev/disk1p0 on /disk1/p0 type fat16\n");
        CHECK(f.out("mount") == "/dev/disk0p0 on /disk0/p0 type fat32\n/dev/disk1p0 on /disk1/p0 type fat16\n");
}

TEST_CASE("Files: redirection, globbing and scripts report errors as bash does") {
        Fixture f;
        CHECK(f.out("cd /disk0/p0") == "");
        CHECK(f.out("echo a > f; set -C; echo b > f", Shell::ExitFailure) == "f: cannot overwrite existing file\n");
        CHECK(f.out("echo c >| f; set +C; cat f") == "c\n");
        CHECK(f.out("x='a b'; echo > $x", Shell::ExitFailure) == "$x: ambiguous redirect\n");
        CHECK(f.out("echo > \"$nothing\"", Shell::ExitFailure) == ": No such file or directory\n");
        CHECK(f.out("mkdir d; echo > d", Shell::ExitFailure) == "d: Is a directory\n");
        CHECK(f.out("cat < nope", Shell::ExitFailure) == "nope: No such file or directory\n");
        CHECK(f.out("echo > /x", Shell::ExitFailure) == "/x: Permission denied\n");
        CHECK(f.out("touch a.txt b.txt; echo > *.txt", Shell::ExitFailure) == "*.txt: ambiguous redirect\n");
        CHECK(f.out("source", Shell::ExitUsage) == "source: filename argument required\n");
        CHECK(f.out(". nope", Shell::ExitFailure) == ".: nope: No such file or directory\n");
        CHECK(f.out("sh nope", Shell::ExitNotFound) == "sh: nope: No such file or directory\n");
        CHECK(f.out("return", Shell::ExitFailure) == "return: can only `return' from a function or sourced script\n");
        CHECK(f.out("rm -f *.o nope") == ""); // nothing matched: nothing to say
        // Globs reach into directories and across mounts; `echo` of nothing matched is the word.
        CHECK(f.out("echo /disk*/p0/d /disk0/p0/*.txt") == "/disk0/p0/d /disk0/p0/a.txt /disk0/p0/b.txt\n");
        CHECK(f.out("echo /dev/n*") == "/dev/null\n");
        CHECK(f.out("echo ~/*.txt") == "/disk0/p0/a.txt /disk0/p0/b.txt\n");
        // `test -x`: directories and scripts.
        CHECK(f.out("touch s.sh; [ -x s.sh ] && [ -x d ] && [ ! -x a.txt ] && echo yes") == "yes\n");
        // A sourced script sees the directory it was run from, and can change it.
        put("/disk0/p0/d/go.sh", "cd /disk0/p0/d\necho \"$PWD\"\n");
        CHECK(f.out(". d/go.sh; pwd; cd ..") == "/disk0/p0/d\n/disk0/p0/d\n");
        CHECK(f.out("sh d/go.sh; pwd") == "/disk0/p0/d\n/disk0/p0\n");
}

TEST_CASE("Files: Tab completes paths") {
        Fixture f;
        CHECK(f.out("cd /disk0/p0; mkdir docs; touch 'my file.txt' notes.txt .hidden") == "");
        // What the editor echoes: the keys, then the completion.
        SUBCASE("a directory gets a '/' and no space") {
                CHECK(f.type("ls do\t") == "ls docs/");
        }
        SUBCASE("a file gets a space; special characters are escaped") {
                CHECK(f.type("cat my\t") == "cat my\\ file.txt ");
                CHECK(f.type("cat my\\ f\t") == "cat my\\ file.txt ");
        }
        SUBCASE("candidates are listed by name; dot files only when asked") {
                CHECK(f.out("touch /disk0/p0/aaa") == ""); // made last, listed first
                CHECK(f.type("cat /disk0/p0/\t") == "cat /disk0/p0/\naaa           docs/         my\\ file.txt  "
                                                    "notes.txt     \nbmds> cat /disk0/p0/");
                CHECK(f.type("cat .h\t") == "cat .hidden ");
        }
        SUBCASE("after a redirection, in command position with a '/', and with ~") {
                CHECK(f.type("echo >no\t") == "echo >notes.txt ");
                CHECK(f.type("./do\t") == "./docs/");
                CHECK(f.type("cat ~/no\t") == "cat ~/notes.txt ");
        }
        SUBCASE("devices and mount points") {
                CHECK(f.type("cat /dev/nu\t") == "cat /dev/null ");
                CHECK(f.type("cd /disk0/\t") == "cd /disk0/p0/");
        }
        SUBCASE("commands on $PATH: links, .elf and .sh by the name that runs them, each once") {
                CHECK(f.out("mkdir bin; touch bin/zzhexedit bin/zztool.elf bin/zzrun.sh bin/cat; mkdir bin/zzdir") ==
                      "");
                CHECK(f.out("ln -s zzhexedit bin/zzhexdump; PATH=/disk0/p0/bin") == "");
                CHECK(f.type("zzhexe\t") == "zzhexedit ");
                CHECK(f.type("zzhexd\t") == "zzhexdump ");
                CHECK(f.type("zzt\t") == "zztool ");
                CHECK(f.type("zzr\t") == "zzrun ");
                CHECK(f.type("cat\t") == "cat "); // the built-in and bin/cat: one candidate
                CHECK(f.type("echo | zzhex\t") == "echo | zzhex\nzzhexdump  zzhexedit  \nbmds> echo | zzhex");
                CHECK(f.type("zzd\t") == "zzd\a"); // a directory isn't a command
        }
}

TEST_CASE("Files: a startup script") {
        Fixture f;
        CHECK(f.shell->sourceIfExists("/disk0/p0/autoexec.sh") == 0);
        CHECK(f.con.take() == "");
        put("/disk0/p0/autoexec.sh", "greeting=hello\ncd /disk0\necho started $#\n[ -e nope ]\n");
        CHECK(f.shell->sourceIfExists("/disk0/p0/autoexec.sh") == 1);
        CHECK(f.con.take() == "started 0\n");
        CHECK(f.out("echo $greeting; pwd") == "hello\n/disk0\n");
        // Ctrl-C stops it, and the shell goes on.
        put("/disk0/p0/autoexec.sh", "while [ 1 ]; do [ 1 ]; done\necho never\n");
        std::thread ctrlC([&f] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                f.con.interrupt().raise();
        });
        CHECK(f.shell->sourceIfExists("/disk0/p0/autoexec.sh") == Shell::ExitInterrupted);
        ctrlC.join();
        CHECK(f.con.take() == "^C\n");
        CHECK(f.out("echo after") == "after\n");
}

TEST_CASE("Files: symbolic links") {
        Fixture f;
        CHECK(f.out("cd /disk0/p0; mkdir bin data; echo BB > bin/busybox; echo n > data/notes") == "");
        // ln -s: one name, or several targets into a directory.
        CHECK(f.out("ln -s busybox bin/awk; ln -s ../data bin/d; ln -s busybox busybox bin; echo $?") ==
              "ln: bin/busybox: File exists\nln: bin/busybox: File exists\n1\n");
        CHECK(f.out("ln busybox bin/x", 1) == "ln: hard links: Operation not supported (FAT has none; use ln -s)\n");
        CHECK(f.out("ln -s data", Shell::ExitUsage).find("usage") != std::string::npos);
        CHECK(f.out("ln -sf other bin/awk; ln -sf busybox bin/awk; cat bin/awk") == "BB\n");
        // ls: shown (not hidden as a System file), as links with -l.
        CHECK(f.out("ls -1 bin") == "awk\nbusybox\nd\n");
        const std::string l = f.out("ls -l bin");
        CHECK(l.find("lrw-") != std::string::npos);
        CHECK(l.find("awk -> busybox") != std::string::npos);
        CHECK(l.find("d -> ../data") != std::string::npos);
        CHECK(f.out("ls bin/d") == "notes\n");
        // test -L/-h; cd takes the physical path.
        CHECK(f.out("[ -L bin/awk ] && [ -h bin/d ] && [ ! -L bin/busybox ] && [ -f bin/awk ] && echo yes") == "yes\n");
        CHECK(f.out("cd bin/d; pwd; cd /disk0/p0") == "/disk0/p0/data\n");
        // cp -r copies links in a tree as links; rm -r removes a link, never what it leads to.
        CHECK(f.out("cp -r bin copy; [ -L copy/awk ] && cat copy/awk") == "BB\n");
        CHECK(f.out("rm -r copy/d; ls data; rm bin/d; ls bin") == "notes\nawk  busybox\n");
        CHECK(f.out("rm -r copy; ls") == "bin/  data/\n");
}

namespace {

        std::string copyName(const char *name, unsigned n) {
                char buf[64];
                auto [v, err] = bmds::CopyName::make(name, n, bmds::Span<char>(buf, sizeof(buf)));
                return err.isOk() ? std::string(v.data(), v.size()) : std::string("error: ") + err.desc();
        }

        std::string read(const char *path) {
                auto [h, err] = Vfs::open(path, Vfs::Read, Vfs::Context{"/", nullptr});
                if (err.isError()) return "<" + std::string(err.desc()) + ">";
                std::string out;
                char        buf[64];
                for (;;) {
                        auto [n, rerr] = h->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                 bmds::WaitForever);
                        if (rerr.isError()) return out;
                        out.append(buf, n);
                }
        }

} // namespace

TEST_CASE("Files: copies' names, paths in URIs, trash info files") {
        CHECK(copyName("terminal.desktop", 1) == "terminal (copy).desktop");
        CHECK(copyName("terminal.desktop", 2) == "terminal (copy 2).desktop");
        CHECK(copyName("archive.tar.gz", 1) == "archive.tar (copy).gz");
        CHECK(copyName(".profile", 1) == ".profile (copy)");
        CHECK(copyName("README", 12) == "README (copy 12)");
        CHECK(copyName("a", 0).rfind("error", 0) == 0);
        CHECK(copyName("a", 1000).rfind("error", 0) == 0);
        CHECK(copyName("a/b", 1).rfind("error", 0) == 0);
        CHECK(copyName("", 1).rfind("error", 0) == 0);
        char small[8];
        CHECK(bmds::CopyName::make("long name.txt", 1, bmds::Span<char>(small, sizeof(small))).error() ==
              Error::BufferTooSmall);

        char buf[128];
        auto [n, err] = bmds::Uri::encodePath("/boot/a b/ü%.txt", bmds::Span<char>(buf, sizeof(buf)));
        REQUIRE(err.isOk());
        CHECK(std::string(buf, n) == "/boot/a%20b/%C3%BC%25.txt");
        auto [m, derr] = bmds::Uri::decode("/boot/a%20b/%C3%BC%25.txt%4", bmds::Span<char>(buf, sizeof(buf)));
        REQUIRE(derr.isOk());
        CHECK(std::string(buf, m) == "/boot/a b/ü%.txt%4");

        bmds::DateTime when;
        when.year = 2026;
        when.month = 10;
        when.day = 7;
        when.hour = 9;
        when.minute = 5;
        when.second = 3;
        char text[256];
        auto [info, ferr] =
                bmds::TrashInfo::format("/boot/desktop/my file", when, bmds::Span<char>(text, sizeof(text)));
        REQUIRE(ferr.isOk());
        CHECK(std::string(info.data(), info.size()) ==
              "[Trash Info]\nPath=/boot/desktop/my%20file\nDeletionDate=2026-10-07T09:05:03\n");
        char path[64];
        char date[bmds::TrashInfo::DateBytes + 1];
        CHECK(bmds::TrashInfo::parse(info, bmds::Span<char>(path, sizeof(path)), date) == Error::Ok);
        CHECK(std::string(path) == "/boot/desktop/my file");
        CHECK(std::string(date) == "2026-10-07T09:05:03");
        CHECK(bmds::TrashInfo::parse("[Trash Info]\nPath=relative\n", bmds::Span<char>(path, sizeof(path)), date) ==
              Error::CorruptData);
        CHECK(bmds::TrashInfo::parse("[Other]\nPath=/x\n", bmds::Span<char>(path, sizeof(path)), date) ==
              Error::CorruptData);
        CHECK(bmds::TrashInfo::parse("[Trash Info]\nPath=/x\n", bmds::Span<char>(path, sizeof(path)), date) ==
              Error::Ok);
        CHECK(std::string(date).empty());
}

TEST_CASE("Files: copyFile") {
        Fixture            f;
        const Vfs::Context root;
        put("/disk0/p0/src.txt", "contents");
        CHECK(Vfs::copyFile("/disk0/p0/src.txt", "/disk1/p0/dst.txt", root) == Error::Ok);
        CHECK(read("/disk1/p0/dst.txt") == "contents");
        CHECK(Vfs::stat("/disk1/p0/dst.txt", root).value().modified.ns ==
              Vfs::stat("/disk0/p0/src.txt", root).value().modified.ns);
        CHECK(Vfs::copyFile("/disk0/p0/src.txt", "/disk1/p0/dst.txt", root) == Error::Exists);
        CHECK(Vfs::copyFile("/disk0/p0/src.txt", "/disk0/p0/src.txt", root) == Error::Exists); // onto itself
        CHECK(read("/disk0/p0/src.txt") == "contents");
        CHECK(Vfs::copyFile("/disk0/p0/none", "/disk1/p0/x", root) == Error::NotExist);
        CHECK(Vfs::copyFile("/disk0/p0", "/disk1/p0/x", root) == Error::IsDir);
}

TEST_CASE("Files: the trash") {
        Fixture            f;
        const Vfs::Context root;
        put("/disk0/p0/a.txt", "first");
        bmds::String name;
        REQUIRE(bmds::Trash::move("/disk0/p0/a.txt", root, &name) == Error::Ok);
        CHECK(name.view() == "a.txt");
        CHECK(read("/disk0/p0/a.txt") == "<" + std::string(Error(Error::NotExist).desc()) + ">");
        CHECK(read("/disk0/p0/.trash/files/a.txt") == "first");
        CHECK(read("/disk0/p0/.trash/info/a.txt.trashinfo").rfind("[Trash Info]\nPath=/disk0/p0/a.txt\n", 0) == 0);
        // The same name again: " (2)".
        put("/disk0/p0/a.txt", "second");
        REQUIRE(bmds::Trash::move("/disk0/p0/a.txt", root, &name) == Error::Ok);
        CHECK(name.view() == "a.txt (2)");
        // A directory, on the other disk (its own trash), named relative to the cwd.
        REQUIRE(Vfs::mkdir("/disk1/p0/dir", root) == Error::Ok);
        put("/disk1/p0/dir/inner", "x");
        REQUIRE(bmds::Trash::move("dir", Vfs::Context{"/disk1/p0", nullptr}, &name) == Error::Ok);
        CHECK(read("/disk1/p0/.trash/files/dir/inner") == "x");
        // Not trashable.
        CHECK(bmds::Trash::move("/disk0/p0", root, nullptr) == Error::Invalid);
        CHECK(bmds::Trash::move("/disk0/p0/.trash", root, nullptr) == Error::Invalid);
        CHECK(bmds::Trash::move("/disk0/p0/.trash/files/a.txt", root, nullptr) == Error::Invalid);
        CHECK(bmds::Trash::move("/disk0/p0/none", root, nullptr) == Error::NotExist);
        CHECK(bmds::Trash::move("/bin/ls", root, nullptr) == Error::NotSupported);

        // trash -l, then -r: the first back; the second refused (its place is taken), then by path.
        const std::string listed = f.out("trash -l");
        CHECK(listed.find("a.txt                     /disk0/p0/a.txt\n") != std::string::npos);
        CHECK(listed.find("a.txt (2)                 /disk0/p0/a.txt\n") != std::string::npos);
        CHECK(listed.find("dir                       /disk1/p0/dir/\n") != std::string::npos);
        CHECK(f.out("trash -r a.txt") == "");
        CHECK(read("/disk0/p0/a.txt") == "first");
        CHECK(f.out("trash -r 'a.txt (2)'", Shell::ExitFailure) == "trash: a.txt (2): its place is taken\n");
        CHECK(f.out("trash -r nothing", Shell::ExitFailure) == "trash: nothing: No such file or directory\n");
        // An info file without its file (a cut-off delete) isn't listed.
        put("/disk0/p0/.trash/info/ghost.trashinfo", "[Trash Info]\nPath=/disk0/p0/ghost\n");
        CHECK(f.out("trash -l").find("ghost") == std::string::npos);

        // trash PATH, then -e for one mount, then all.
        put("/disk0/p0/b.txt", "b");
        CHECK(f.out("cd /disk0/p0; trash b.txt; cd /") == "");
        CHECK(f.out("trash /disk0/p0/nope", Shell::ExitFailure) ==
              "trash: /disk0/p0/nope: No such file or directory\n");
        CHECK(f.out("trash /bin/ls", Shell::ExitFailure) == "trash: /bin/ls: not on a filesystem with a trash\n");
        CHECK(f.out("trash -e /disk1/p0/") == "");
        CHECK(f.out("trash -l").find("/disk1") == std::string::npos);
        CHECK(f.out("trash -l").find("b.txt") != std::string::npos);
        CHECK(f.out("trash -e") == "");
        CHECK(f.out("trash -l") == "");
        CHECK(read("/disk0/p0/a.txt") == "first"); // restored ones stay
        CHECK(f.out("trash -e /nowhere", Shell::ExitFailure) == "trash: /nowhere: No such file or directory\n");
        CHECK(f.out("trash", Shell::ExitUsage).rfind("usage: trash", 0) == 0);
        CHECK(f.out("trash -x", Shell::ExitUsage).rfind("usage: trash", 0) == 0);
}

TEST_CASE("Vfs::identify: by name, by contents, the flags") {
        Fixture            f;
        const Vfs::Context vc{"/", nullptr};
        const auto         type = [&](const char *path, uint32_t flags = 0) {
                auto [t, err] = Vfs::identify(path, flags, vc);
                return err.isOk() ? std::string(t.data(), t.size()) : std::string("error: ") + err.desc();
        };
        put("/disk0/p0/notes.txt", std::string("\x7f"
                                               "ELF",
                                               4)); // the name wins, unread
        put("/disk0/p0/prog", std::string("\x7f"
                                          "ELF\x01\x01\x01\0",
                                          8));
        put("/disk0/p0/script", "#!/bin/sh\necho hi\n");
        put("/disk0/p0/empty", "");
        std::string big(600, 'a');
        big[511] = '\xc3'; // a character cut by the 512 bytes read: still text
        big[512] = '\xa9';
        put("/disk0/p0/long", big);
        CHECK(f.out("mkdir /disk0/p0/dir.png") == "");
        CHECK(type("/disk0/p0/notes.txt") == "text/plain");
        CHECK(type("/disk0/p0/notes.txt", Vfs::IdentifyContent) == "application/x-executable");
        CHECK(type("/disk0/p0/prog") == "application/x-executable");
        CHECK(type("/disk0/p0/script") == "application/x-shellscript");
        CHECK(type("/disk0/p0/empty") == "application/x-zerosize");
        CHECK(type("/disk0/p0/long") == "text/plain");
        CHECK(type("/disk0/p0/dir.png", Vfs::IdentifyContent) == "inode/directory");
        CHECK(type("/dev/null") == "inode/chardevice");
        // By the name only: nothing read, so a file not there still has one.
        CHECK(type("/disk0/p0/nothing.c", Vfs::IdentifyName) == "text/x-csrc");
        CHECK(type("/disk0/p0/prog", Vfs::IdentifyName) == "application/octet-stream");
        CHECK(type("/disk0/p0/nothing") == "error: Does not exist");
}
