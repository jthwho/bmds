/**
 * @file      launch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The shell's side of running files: $PATH lookup, dispatch by content
 * (programs, `#!` and `.sh` scripts), what a ProgramLauncher is given and
 * what its results become. On a RAM disk, with stand-in "programs" (files
 * that start with the ELF magic; the fake launcher runs nothing).
 */

#include <doctest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/builtinprograms.h>
#include <bmds/filecommands.h>
#include <bmds/langbuiltins.h>
#include <bmds/programlauncher.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"

using bmds::Error;
using bmds::Shell;
namespace Vfs = bmds::Vfs;

namespace {

        bmds::StringView sv(const std::string &s) {
                return bmds::StringView(s.data(), s.size());
        }

        // Records each request; runs nothing.
        class FakeLauncher final : public bmds::ProgramLauncher {
                public:
                        bmds::Result<int> run(const Request &r) override {
                                std::lock_guard<std::mutex> lock(_lock);
                                Call                        c;
                                c.path.assign(r.path.data(), r.path.size());
                                for (bmds::StringView a : r.args) c.args.emplace_back(a.data(), a.size());
                                for (bmds::StringView e : r.environment) c.env.emplace_back(e.data(), e.size());
                                c.cwd.assign(r.cwd.data(), r.cwd.size());
                                c.console = r.console;
                                c.interrupt = r.interrupt;
                                calls.push_back(c);
                                // Ctrl-C while it runs (the loader would send it SIGINT).
                                if (pressCtrlC && r.interrupt != nullptr) r.interrupt->raise();
                                // The program "writes" to its stdout.
                                bmds::FileHandle *out = r.fds->handle(1);
                                if (out != nullptr) (void)out->write(bmds::StringView("ran\n"), bmds::WaitForever);
                                if (fail.isError()) return bmds::makeError<int>(fail);
                                return bmds::makeResult(status);
                        }

                        struct Call {
                                        std::string              path;
                                        std::vector<std::string> args;
                                        std::vector<std::string> env;
                                        std::string              cwd;
                                        bmds::Console           *console = nullptr;
                                        bmds::Interrupt         *interrupt = nullptr;
                        };
                        std::vector<Call> calls;
                        int               status = 0;
                        Error             fail;
                        bool              pressCtrlC = false;

                private:
                        std::mutex _lock;
        };

        namespace F = bmds::FileCommands;
        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command Commands[] = {
                {"[", "", "", L::test},           {"cd", "", "", F::cd},          {"command", "", "", L::command},
                {"echo", "", "", T::echo},        {"env", "", "", L::env},        {"exit", "", "", L::exitCmd},
                {"export", "", "", L::exportCmd}, {"false", "", "", L::falseCmd}, {"sh", "", "", L::sh},
                {"type", "", "", L::type},        {"wc", "", "", T::wc},
        };

        int unusedMain(const bmds::BuiltinProgram::Context &) {
                return 0;
        }

        // /bin for the tests (sorted by name): sh and env are shell commands (run in the shell),
        // bash isn't (the launcher runs it).
        constexpr bmds::ProgramEntry BinPrograms[] = {
                {"bash", {unusedMain, 4096}},
                {"env", {unusedMain, 4096}},
                {"sh", {unusedMain, 4096}},
        };

        // A RAM disk at /<name>/p0 for the files, removed afterwards (the
        // registry and mounts are global).
        struct Fixture {
                        Fixture() : disk(512) {
                                REQUIRE(disk.allocate(4 * 2048, bmds::MemSpace::Psram).isOk());
                                REQUIRE(Vfs::init().isOk());
                                bmds::BuiltinPrograms::detail::setForHost(
                                        bmds::Span<const bmds::ProgramEntry>(BinPrograms));
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(disk, "ldisk", name).isOk());
                                root = std::string("/") + name + "/p0";
                                REQUIRE(Vfs::format(disk, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::automount(disk).value() == 1);
                                shell = std::make_unique<Shell>(con, bmds::Span<const bmds::Command>(Commands),
                                                                1 << 20);
                                REQUIRE(shell->init().isOk());
                                shell->setLauncher(&launcher);
                                REQUIRE(Vfs::mkdir(sv(root + "/bin"), ctx()).isOk());
                                REQUIRE(Vfs::mkdir(sv(root + "/dir"), ctx()).isOk());
                                REQUIRE(Vfs::mkdir(sv(root + "/other"), ctx()).isOk());
                                REQUIRE(shell->execute(("cd " + root).c_str()) == 0);
                        }

                        ~Fixture() {
                                bmds::BuiltinPrograms::detail::setForHost(bmds::Span<const bmds::ProgramEntry>());
                                shell.reset();
                                Vfs::eject(disk);
                                bmds::BlockDevices::remove(disk);
                        }

                        static Vfs::Context ctx() { return Vfs::Context{"/", nullptr}; }

                        // A file under the root.
                        void put(const std::string &path, const std::string &text) {
                                auto [h, err] = Vfs::open(sv(root + "/" + path),
                                                          Vfs::Write | Vfs::Create | Vfs::Truncate, ctx());
                                REQUIRE_MESSAGE(err.isOk(), path);
                                REQUIRE(h->write(bmds::StringView(text.data(), text.size()), bmds::WaitForever)
                                                .error()
                                                .isOk());
                        }

                        // A stand-in program: the magic is all the shell looks at.
                        void program(const std::string &path) { put(path, std::string("\x7f" "ELF\x01\x01\x01", 7)); }

                        std::string out(const std::string &line, int expect) {
                                CHECK_MESSAGE(shell->execute(line.c_str()) == expect, line);
                                return con.take();
                        }

                        const FakeLauncher::Call &last() {
                                REQUIRE_FALSE(launcher.calls.empty());
                                return launcher.calls.back();
                        }

                        bmds::RamBlockDevice   disk;
                        std::string            root;
                        FakeConsole            con;
                        FakeLauncher           launcher;
                        std::unique_ptr<Shell> shell;
        };

        bool has(const std::vector<std::string> &v, const std::string &s) {
                for (const std::string &x : v) {
                        if (x == s) return true;
                }
                return false;
        }

        using Args = std::vector<std::string>;

} // namespace

TEST_CASE("Launch: a program by path") {
        Fixture f;
        f.program("prog");
        f.launcher.status = 3;
        CHECK(f.out("export SHOWN=yes; HIDDEN=no; ./prog one 'two words'; echo $?", 0) == "ran\n3\n");
        REQUIRE(f.launcher.calls.size() == 1);
        const auto &c = f.launcher.calls[0];
        CHECK(c.path == "./prog");
        CHECK(c.args == Args{"./prog", "one", "two words"});
        CHECK(has(c.env, "SHOWN=yes"));
        CHECK(has(c.env, "TERM=xterm-256color"));
        CHECK_FALSE(has(c.env, "HIDDEN=no"));
        CHECK(c.cwd == f.root);
        CHECK(c.console == &f.con); // the terminal, for /dev/console and the terminal calls
        CHECK(c.interrupt == &f.con.interrupt());
        // A prefix assignment reaches the program, for that command only.
        CHECK(f.out("ONCE=1 " + f.root + "/prog", 3) == "ran\n");
        CHECK(has(f.last().env, "ONCE=1"));
        // Programs work in pipelines and with redirections like any command.
        f.launcher.status = 0;
        CHECK(f.out("./prog | wc -l", 0) == "1\n");
        CHECK(f.out("./prog > /dev/null", 0) == "");
}

TEST_CASE("Launch: Ctrl-C abandons the command line only if it ended the program") {
        Fixture f;
        f.program("prog");
        f.launcher.pressCtrlC = true;
        // Ended by SIGINT (128 + 2): the rest of the line is abandoned, as in bash.
        f.launcher.status = Shell::ExitInterrupted;
        CHECK(f.out("./prog; echo next", Shell::ExitInterrupted) == "ran\n^C\n");
        // Caught or ignored: the program's status, and the shell goes on.
        f.launcher.status = 7;
        CHECK(f.out("./prog; echo next $?", 0) == "ran\nnext 7\n");
        CHECK_FALSE(f.con.interrupt().isRaised());
}

TEST_CASE("Launch: failures become 127 and 126") {
        Fixture f;
        f.program("prog");
        f.put("notes.txt", "just text\n");
        f.put("empty", "");
        CHECK(f.out("./nope", Shell::ExitNotFound) == "./nope: No such file or directory\n");
        CHECK(f.out("./dir", Shell::ExitCannotRun) == "./dir: Is a directory\n");
        CHECK(f.out("./notes.txt", Shell::ExitCannotRun) == "./notes.txt: cannot execute: not a program or script\n");
        CHECK(f.out("./empty", Shell::ExitCannotRun) == "./empty: cannot execute: not a program or script\n");
        CHECK(f.launcher.calls.empty());
        // The launcher's own refusals (it reports them itself).
        f.launcher.fail = Error::NotExist;
        CHECK(f.out("./prog", Shell::ExitNotFound) == "ran\n");
        f.launcher.fail = Error::NotSupported;
        CHECK(f.out("./prog", Shell::ExitCannotRun) == "ran\n");
        // Without a launcher, programs can't run (scripts still can).
        f.shell->setLauncher(nullptr);
        CHECK(f.out("./prog", Shell::ExitCannotRun) == "./prog: cannot execute: not a program or script\n");
}

TEST_CASE("Launch: $PATH") {
        Fixture f;
        f.program("bin/hello.elf");
        f.program("other/hello"); // later on $PATH: hidden
        f.program("bin/both");
        f.program("bin/both.elf");
        f.put("bin/tool.sh", "echo tool $#\n");
        f.program("bin/echo"); // built-ins come first
        f.program("local");
        REQUIRE(f.out("PATH=" + f.root + "/bin:" + f.root + "/other", 0) == "");
        const std::string bin = f.root + "/bin";

        // name, name.elf, name.sh in each directory; argv[0] as typed.
        CHECK(f.out("hello a b", 0) == "ran\n");
        CHECK(f.last().path == bin + "/hello.elf");
        CHECK(f.last().args == Args{"hello", "a", "b"});
        CHECK(f.out("both", 0) == "ran\n");
        CHECK(f.last().path == bin + "/both");
        CHECK(f.out("hello.elf", 0) == "ran\n");
        CHECK(f.out("tool x y", 0) == "tool 2\n");
        CHECK(f.out("echo hi", 0) == "hi\n");
        CHECK(f.out("nope", Shell::ExitNotFound) == "nope: command not found\n");
        CHECK(f.out("local", Shell::ExitNotFound) == "local: command not found\n");
        // An empty entry is the current directory.
        CHECK(f.out("PATH=" + bin + ":; local", 0) == "ran\n");
        CHECK(f.last().path == "./local");
        // Without $PATH, only paths run.
        CHECK(f.out("unset_path() { PATH=; hello; }; unset_path", Shell::ExitNotFound) ==
              "hello: command not found\n");

        // What `type` and `command` say, and `command` skipping functions.
        CHECK(f.out("PATH=" + bin, 0) == "");
        CHECK(f.out("type hello; type -t hello; command -v hello tool", 0) ==
              "hello is " + bin + "/hello.elf\nfile\n" + bin + "/hello.elf\n" + bin + "/tool.sh\n");
        CHECK(f.out("type ./local", 0) == "./local is ./local\n");
        CHECK(f.out("type nope", 1) == "type: nope: not found\n");
        CHECK(f.out("hello() { echo function; }; hello; command hello x", 0) == "function\nran\n");
        CHECK(f.last().args == Args{"hello", "x"});
        CHECK(f.out("command nope", Shell::ExitNotFound) == "nope: command not found\n");
}

TEST_CASE("Launch: .sh scripts") {
        Fixture f;
        f.put("s.sh", "echo \"$# [$1] [$2]\"\nset_here=1\nexit 3\n");
        // Run in a subshell, with the arguments as positional parameters.
        CHECK(f.out("./s.sh a 'b c'; echo $? ${set_here-unset}", 0) == "2 [a] [b c]\n3 unset\n");
        CHECK(f.out("dir/../s.sh", 3) == "0 [] []\n");
        // A program named .sh is still a program.
        f.program("p.sh");
        CHECK(f.out("./p.sh", 0) == "ran\n");
}

TEST_CASE("Launch: #! scripts") {
        Fixture f;
        f.program("bin/interp.elf");
        f.program("bin/hello.elf");
        REQUIRE(f.out("PATH=" + f.root + "/bin", 0) == "");
        const std::string bin = f.root + "/bin";

        // The programs built in (/bin), as named, by the launcher: the shell itself is a process
        // (devplan/processes.md §3.4)...
        f.put("a", "#!/bin/sh\necho \"sh $# $1\"\n");
        CHECK(f.out("./a x", 0) == "ran\n");
        CHECK(f.last().path == "/bin/sh");
        CHECK(f.last().args == Args{"/bin/sh", "./a", "x"});
        // ...and any other, with its argument.
        f.put("b", "#!/bin/bash -e\nfalse\n");
        CHECK(f.out("./b", 0) == "ran\n");
        CHECK(f.last().path == "/bin/bash");
        CHECK(f.last().args == Args{"/bin/bash", "-e", "./b"});
        // /bin/env looks the name up as a command ($PATH too).
        f.put("c", "#!/bin/env sh\necho env-sh $1\n");
        CHECK(f.out("./c y", 0) == "env-sh y\n");
        f.put("d", "#!/bin/env hello\r\nanything\n");
        CHECK(f.out("./d 1 2", 0) == "ran\n");
        CHECK(f.last().path == bin + "/hello.elf");
        CHECK(f.last().args == Args{"hello", "./d", "1", "2"});
        f.put("e", "#!/bin/env nope\n");
        CHECK(f.out("./e", Shell::ExitNotFound) == "nope: command not found\n");
        // Paths this system doesn't have aren't guessed.
        f.put("n", "#!/usr/bin/env sh\n");
        CHECK(f.out("./n", Shell::ExitNotFound) == "./n: /usr/bin/env: No such file or directory\n");
        f.put("o", "#!/usr/bin/bash\n");
        CHECK(f.out("./o", Shell::ExitNotFound) == "./o: /usr/bin/bash: No such file or directory\n");

        // A program as the interpreter, with its one argument (the rest of the line).
        f.put("g", "#!" + bin + "/interp.elf  -f  two words \nbody\n");
        CHECK(f.out("./g x", 0) == "ran\n");
        CHECK(f.last().path == bin + "/interp.elf");
        CHECK(f.last().args == Args{bin + "/interp.elf", "-f  two words", "./g", "x"});
        // Through $PATH, the script's path as found.
        f.put("bin/tool", "#!" + bin + "/interp.elf\n");
        CHECK(f.out("tool", 0) == "ran\n");
        CHECK(f.last().args == Args{bin + "/interp.elf", bin + "/tool"});
        // A script as the interpreter (nested).
        f.put("bin/wrap.sh", "echo \"wrap $# $1 $2\"\n");
        f.put("h", "#!" + bin + "/wrap.sh\n");
        CHECK(f.out("./h z", 0) == "wrap 2 ./h z\n");
        f.put("i", "#!" + f.root + "/h\n");
        CHECK(f.out("./i", 0) == "wrap 2 " + f.root + "/h ./i\n");

        // Refusals.
        f.put("j", "#!/opt/nothing\n");
        CHECK(f.out("./j", Shell::ExitNotFound) == "./j: /opt/nothing: No such file or directory\n");
        f.put("k", "#!\n");
        CHECK(f.out("./k", Shell::ExitCannotRun) == "./k: cannot execute: no interpreter after #!\n");
        f.put("l", "#!" + std::string(300, 'x') + "\n");
        CHECK(f.out("./l", Shell::ExitCannotRun) == "./l: interpreter line too long (at most 255 bytes)\n");
        f.put("m", "#!/bin/sh"); // no newline: the whole file is the line
        CHECK(f.out("./m", 0) == "ran\n");
        CHECK(f.last().args == Args{"/bin/sh", "./m"});
        f.put("loop", "#!" + f.root + "/loop\n");
        CHECK(f.out("./loop", Shell::ExitCannotRun) == f.root + "/loop: too many levels of interpreters\n");
}

TEST_CASE("Launch: test -x") {
        Fixture f;
        f.program("prog");
        f.put("s.sh", "");
        f.put("script", "#!/bin/sh\n");
        f.put("text", "hello\n");
        CHECK(f.out("for x in prog s.sh script dir text nope; do [ -x $x ] && echo $x; done", 1) ==
              "prog\ns.sh\nscript\ndir\n");
}

TEST_CASE("Launch: env runs a command with variables set, then puts them back") {
        Fixture f;
        CHECK(f.out("X=a; env X=b sh -c 'echo $X'; echo $X", 0) == "b\na\n");
        CHECK(f.out("env Y=1 echo; echo ${Y-unset}", 0) == "\nunset\n");
        CHECK(f.out("env Z=1 Z=2 sh -c 'echo $Z'; echo ${Z-unset}", 0) == "2\nunset\n");
        const std::string listed = f.out("export W=w; V=hidden; env", 0);
        CHECK(listed.find("W=w\n") != std::string::npos);
        CHECK(listed.find("V=") == std::string::npos); // not exported
        CHECK(f.out("env nope", Shell::ExitNotFound) == "nope: command not found\n");
}
