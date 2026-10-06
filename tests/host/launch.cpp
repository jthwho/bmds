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

#include <map>
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

        // The shell's process: its children are the jobs the launcher started. A child that
        // hasn't ended ends with its `status` when waited for.
        class FakeProcess final : public bmds::ProcessControl {
                public:
                        struct Child {
                                        bool    ended = false;
                                        int32_t status = 0; // a wait status
                                        int32_t change = 0; // a stop or continue to report (its wait status)
                        };

                        int32_t processId() const override { return 7; }

                        Error setSignalAction(int, SignalAction) override { return Error::Ok; }

                        bool isEnding() const override { return false; }

                        bmds::Result<int32_t> waitChild(int32_t pid, uint32_t flags, int32_t &status) override {
                                auto it = children.find(pid);
                                if (it == children.end()) return bmds::makeError<int32_t>(Error::NotExist);
                                Child &c = it->second;
                                // A stop or continue to report first, when asked for.
                                if (c.change != 0 && ((c.change & 0xff) == 0x7f ? (flags & WaitStopped) != 0
                                                                                : (flags & WaitContinued) != 0)) {
                                        status = c.change;
                                        c.change = 0;
                                        return bmds::makeResult(pid);
                                }
                                const bool block = (flags & WaitBlock) != 0;
                                if (!c.ended && !block) return bmds::makeResult<int32_t>(0);
                                if (!c.ended && interruptWaits) return bmds::makeError<int32_t>(Error::Interrupted);
                                status = c.status;
                                children.erase(it);
                                return bmds::makeResult(pid);
                        }

                        void disownChild(int32_t pid) override {
                                disowned.push_back(pid);
                                children.erase(pid);
                        }

                        Error signal(int32_t target, int sig) override {
                                signals.push_back({target, sig});
                                // SIGCONT to a stopped job's group: what `fg` waits for next.
                                if (sig == 18 && target < 0) {
                                        auto it = children.find(-target);
                                        if (it != children.end() && afterContinue != 0) {
                                                it->second.change = afterContinue == -1 ? 0 : afterContinue;
                                                if (afterContinue != -1 && (afterContinue & 0xff) != 0x7f) {
                                                        it->second.ended = true;
                                                        it->second.status = afterContinue;
                                                        it->second.change = 0;
                                                }
                                        }
                                }
                                return Error::Ok;
                        }

                        Error setForeground(int32_t group) override {
                                foregrounds.push_back(group);
                                return Error::Ok;
                        }

                        std::map<int32_t, Child>             children;
                        std::vector<int32_t>                 disowned;
                        std::vector<std::pair<int32_t, int>> signals;
                        std::vector<int32_t>                 foregrounds;
                        bool                                 interruptWaits = false;
                        int32_t afterContinue = 0; // what a continued job does next: a stop or an end
        };

        // Records each request; runs nothing, except kernel code a start names (a background
        // subshell), on the caller's thread before start() returns.
        class FakeLauncher final : public bmds::ProgramLauncher {
                public:
                        bmds::Result<int> run(const Request &r) override {
                                std::lock_guard<std::mutex> lock(_lock);
                                calls.push_back(record(r));
                                // Ctrl-Z while it runs: it stops, and stays the caller's child.
                                if (stopNext && r.stopped != nullptr && process != nullptr) {
                                        stopNext = false;
                                        const int32_t pid = nextPid++;
                                        process->children[pid] = FakeProcess::Child();
                                        *r.stopped = pid;
                                        return bmds::makeResult(128 + 20);
                                }
                                // Ctrl-C while it runs (the loader would send it SIGINT).
                                if (pressCtrlC && r.interrupt != nullptr) r.interrupt->raise();
                                // The program "writes" to its stdout.
                                bmds::FileHandle *out = r.fds->handle(1);
                                if (out != nullptr) (void)out->write(bmds::StringView("ran\n"), bmds::WaitForever);
                                if (fail.isError()) return bmds::makeError<int>(fail);
                                return bmds::makeResult(status);
                        }

                        bmds::Result<int32_t> start(const Request &r) override {
                                int32_t pid = 0;
                                {
                                        std::lock_guard<std::mutex> lock(_lock);
                                        Call                        c = record(r);
                                        c.started = true;
                                        c.waited = r.waited;
                                        c.subshell = r.builtin != nullptr;
                                        c.group = r.group != nullptr;
                                        c.foreground = r.group != nullptr && r.group->foreground;
                                        calls.push_back(c);
                                        pid = nextPid++;
                                }
                                if (process != nullptr && r.waited) process->children[pid] = FakeProcess::Child();
                                if (r.builtin == nullptr) return bmds::makeResult(pid);
                                bmds::BuiltinProgram::Context context;
                                context.fds = r.fds;
                                context.cwd = r.cwd;
                                context.args = r.args;
                                context.environment = r.environment;
                                context.console = r.console;
                                context.launcher = this;
                                context.program = "sh";
                                context.data = r.data;
                                const int code = r.builtin->program.main(context);
                                if (process != nullptr && r.waited) process->children[pid] = {true, (code & 0xff) << 8};
                                // Ctrl-Z while the subshell runs.
                                if (process != nullptr && r.waited && stopNext) {
                                        stopNext = false;
                                        process->children[pid] = {false, 0, (20 << 8) | 0x7f};
                                }
                                return bmds::makeResult(pid);
                        }

                        struct Call {
                                        std::string              path;
                                        std::vector<std::string> args;
                                        std::vector<std::string> env;
                                        std::string              cwd;
                                        bmds::Console           *console = nullptr;
                                        bmds::Interrupt         *interrupt = nullptr;
                                        bool                     started = false;  // start(), not run()
                                        bool                     waited = false;   // start(): the shell's child
                                        bool                     subshell = false; // start(): kernel code
                                        bool                     group = false;    // a group was named
                                        bool                     foreground = false; // ... in the foreground
                                        bool                     stoppable = false;  // run(): a stop ends the wait
                                        bool                     ignoreStops = false;
                        };
                        std::vector<Call> calls;
                        int               status = 0;
                        Error             fail;
                        bool              pressCtrlC = false;
                        bool              stopNext = false; // the next program (or subshell) stops
                        int32_t           nextPid = 100;
                        FakeProcess      *process = nullptr;

                private:
                        static Call record(const Request &r) {
                                Call c;
                                c.path.assign(r.path.data(), r.path.size());
                                for (bmds::StringView a : r.args) c.args.emplace_back(a.data(), a.size());
                                for (bmds::StringView e : r.environment) c.env.emplace_back(e.data(), e.size());
                                c.cwd.assign(r.cwd.data(), r.cwd.size());
                                c.console = r.console;
                                c.interrupt = r.interrupt;
                                c.stoppable = r.stopped != nullptr;
                                c.ignoreStops = r.ignoreStops;
                                return c;
                        }

                        std::mutex _lock;
        };

        namespace F = bmds::FileCommands;
        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command Commands[] = {
                {"[", "", "", L::test},          {"bg", "", "", L::bg},         {"cd", "", "", F::cd},
                {"command", "", "", L::command}, {"disown", "", "", L::disown}, {"echo", "", "", T::echo},
                {"env", "", "", L::env},         {"exit", "", "", L::exitCmd},  {"export", "", "", L::exportCmd},
                {"false", "", "", L::falseCmd},  {"fg", "", "", L::fg},         {"jobs", "", "", L::jobs},
                {"read", "", "", L::read},       {"sh", "", "", L::sh},         {"type", "", "", L::type},
                {"wait", "", "", L::wait},       {"wc", "", "", T::wc},
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

                        // The shell as a process: background jobs work.
                        void beProcess() {
                                launcher.process = &process;
                                shell->setProcess(&process);
                                REQUIRE(shell->execute("PATH=.") == 0);
                        }

                        // An interactive shell's job control (devplan/processes.md §3.7.1).
                        void beInteractive() {
                                beProcess();
                                shell->setJobControl(true);
                        }

                        const FakeLauncher::Call &last() {
                                REQUIRE_FALSE(launcher.calls.empty());
                                return launcher.calls.back();
                        }

                        bmds::RamBlockDevice   disk;
                        std::string            root;
                        FakeConsole            con;
                        FakeLauncher           launcher;
                        FakeProcess            process;
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

TEST_CASE("Jobs: a lone program is started as a job") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        CHECK(f.out("prog a 'b c' &", 0) == "");
        REQUIRE(f.launcher.calls.size() == 1);
        const auto &c = f.launcher.calls[0];
        CHECK(c.started);
        CHECK(c.waited);
        CHECK_FALSE(c.subshell);
        CHECK_FALSE(c.group); // the launcher gives it a group of its own
        CHECK(c.args == Args{"prog", "a", "b c"});
        CHECK(f.out("echo $! $$", 0) == "100 7\n");
        CHECK(f.out("jobs", 0) == "[1]+  Running                 prog a 'b c' &\n");
        CHECK(f.out("jobs -l", 0) == "[1]+ 100 Running                 prog a 'b c' &\n");
        CHECK(f.out("jobs -p", 0) == "100\n");
        // Ended: reported once.
        f.process.children[100] = {true, 3 << 8};
        CHECK(f.out("jobs", 0) == "[1]+  Exit 3                  prog a 'b c'\n");
        CHECK(f.out("jobs", 0) == "");
        // Redirections apply to it; `&` separates commands as `;` does.
        CHECK(f.out("prog > out.txt & echo next", 0) == "next\n");
        CHECK(f.launcher.calls.size() == 2);
}

TEST_CASE("Jobs: anything else is a subshell process") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        CHECK(f.out("f() { echo in f $1; }; x=5", 0) == "");
        // The subshell has the shell's functions and variables (the fake runs it at once).
        CHECK(f.out("f $x && echo after &", 0) == "in f 5\nafter\n");
        const auto &c = f.last();
        CHECK(c.started);
        CHECK(c.waited);
        CHECK(c.subshell);
        CHECK(c.args == Args{"sh", "-c", "f $x && echo after"});
        CHECK(f.out("jobs", 0) == "[1]+  Done                    f $x && echo after\n");
        // Its stdin is /dev/null; its exit status is the job's.
        CHECK(f.out("{ read line; echo \"got [$line] $?\"; } &", 0) == "got [] 1\n");
        CHECK(f.out("exit 4 & wait $!; echo $?", 0) == "4\n");
        // What it changes stays its own.
        CHECK(f.out("x=6 & echo $x", 0) == "5\n");
        // $$ is the shell's in it too.
        CHECK(f.out("echo $$ &", 0) == "7\n");
        // A program in a pipeline or a compound command isn't a lone program.
        f.launcher.calls.clear();
        CHECK(f.out("prog | wc -l &", 0) == "1\n");
        CHECK(f.launcher.calls.size() == 2);
        CHECK(f.launcher.calls[0].subshell);
        CHECK_FALSE(f.launcher.calls[1].started); // run by the subshell
}

TEST_CASE("Jobs: wait") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        CHECK(f.out("prog & prog &", 0) == "");
        f.process.children[100].status = 1 << 8;
        // All of them: status 0, and they are gone.
        CHECK(f.out("wait; echo $?; jobs", 0) == "0\n");
        CHECK(f.process.children.empty());
        // One: its status (a signal's: 128 + it).
        CHECK(f.out("prog &", 0) == "");
        f.process.children[102].status = 5 << 8;
        CHECK(f.out("wait %1; echo $?", 0) == "5\n");
        CHECK(f.out("prog &", 0) == "");
        f.process.children[103].status = 15;
        CHECK(f.out("jobs; wait $!; echo $?", 0) == "[1]+  Running                 prog &\n143\n");
        CHECK(f.out("prog &", 0) == "");
        f.process.children[104] = {true, 9};
        CHECK(f.out("jobs", 0) == "[1]+  Killed                  prog\n");
        // Not jobs.
        CHECK(f.out("wait 99", Shell::ExitNotFound) == "wait: pid 99 is not a child of this shell\n");
        CHECK(f.out("wait %3", Shell::ExitNotFound) == "wait: %3: no such job\n");
        // Interrupted (Ctrl-C): 130, the job stays.
        CHECK(f.out("prog &", 0) == "");
        f.process.interruptWaits = true;
        CHECK(f.out("wait", Shell::ExitInterrupted) == "");
        CHECK(f.out("jobs", 0) == "[1]+  Running                 prog &\n");
}

TEST_CASE("Jobs: specs and disown") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        CHECK(f.out("prog one & prog two & prog three &", 0) == "");
        CHECK(f.out("jobs", 0) == "[1]   Running                 prog one &\n"
                                  "[2]-  Running                 prog two &\n"
                                  "[3]+  Running                 prog three &\n");
        CHECK(f.out("wait %pro", Shell::ExitNotFound) == "wait: %pro: ambiguous job spec\n");
        CHECK(f.out("disown %?wo", 0) == "");
        CHECK(f.process.disowned == std::vector<int32_t>{101});
        CHECK(f.out("jobs", 0) == "[1]-  Running                 prog one &\n"
                                  "[3]+  Running                 prog three &\n");
        CHECK(f.out("disown %-; disown", 0) == "");
        CHECK(f.process.disowned == std::vector<int32_t>{101, 100, 102});
        CHECK(f.out("jobs; disown", Shell::ExitFailure) == "disown: current: no such job\n");
        // Numbers go on from the highest.
        CHECK(f.out("prog & prog & disown %1; prog & jobs -l", 0) == "[2]- 104 Running                 prog &\n"
                                                                     "[3]+ 105 Running                 prog &\n");
}

TEST_CASE("Jobs: a subshell's jobs are disowned when it ends") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        CHECK(f.out("( prog & jobs ); jobs", 0) == "[1]+  Running                 prog &\n");
        CHECK(f.process.disowned == std::vector<int32_t>{100});
}

TEST_CASE("Jobs: here-documents") {
        Fixture f;
        f.program("prog");
        f.beProcess();
        // A lone program gets its here-document as any command does.
        CHECK(f.out("prog <<EOF &\nhi\nEOF", 0) == "");
        CHECK(f.last().started);
        // A subshell parses the job's text again, and the body isn't in it.
        CHECK(f.out("wc -l <<EOF &\nhi\nEOF", Shell::ExitUsage) ==
              "a here-document after a background job (&) is not supported\n");
        // Inside the job's text, it is.
        CHECK(f.out("{ wc -l <<EOF\nhi\nEOF\n} &", 0) == "1\n");
}

TEST_CASE("Stopping: a foreground program that stops is a stopped job") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        f.launcher.stopNext = true;
        // The rest of the command line goes (a loop ends).
        CHECK(f.out("for i in 1 2 3; do prog $i; done; echo after", 148) ==
              "^Z\n[1]+  Stopped                 prog 1\n");
        CHECK(f.launcher.calls.size() == 1);
        CHECK(f.out("echo after", 0) == "after\n");
        CHECK(f.last().stoppable);
        CHECK(f.out("jobs -l", 0) == "[1]+ 100 Stopped                 prog 1\n");
        CHECK(f.out("echo $?", 0) == "0\n");
        // Without job control a program can't be stopped this way.
        f.shell->setJobControl(false);
        CHECK(f.out("prog", 0) == "ran\n");
        CHECK_FALSE(f.last().stoppable);
}

TEST_CASE("Stopping: fg continues a job in the foreground; bg in the background") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        f.launcher.stopNext = true;
        CHECK(f.out("prog a", 148) == "^Z\n[1]+  Stopped                 prog a\n");
        // It stops again: still a job.
        f.process.afterContinue = (20 << 8) | 0x7f;
        CHECK(f.out("fg", 148) == "prog a\n^Z\n[1]+  Stopped                 prog a\n");
        REQUIRE(f.process.signals.size() == 1);
        CHECK(f.process.signals[0] == std::pair<int32_t, int>{-100, 18});
        CHECK(f.process.foregrounds == std::vector<int32_t>{100, 0});
        // bg continues it in the background; once only.
        f.process.afterContinue = -1;
        CHECK(f.out("bg 1", 0) == "[1]+ prog a &\n"); // a bare number is a job number, as bash
        CHECK(f.out("jobs", 0) == "[1]+  Running                 prog a &\n");
        CHECK(f.out("bg", 0) == "bg: job 1 already in background\n");
        // fg until it ends: its status, and the job is gone.
        f.process.children[100].status = 3 << 8;
        CHECK(f.out("fg %prog", 3) == "prog a\n");
        CHECK(f.out("jobs", 0) == "");
        CHECK(f.out("fg", 1) == "fg: current: no such job\n");
        CHECK(f.out("bg %4", 1) == "bg: %4: no such job\n");
        f.shell->setJobControl(false);
        CHECK(f.out("fg", 1) == "fg: no job control\n");
}

TEST_CASE("Stopping: a pipeline ending in a program is one job, a subshell in the foreground") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        CHECK(f.out("echo hi | prog", 0) == "ran\n");
        const FakeLauncher::Call &job = f.launcher.calls[f.launcher.calls.size() - 2];
        CHECK(job.subshell);
        CHECK(job.foreground);
        CHECK(job.args == Args{"sh", "-c", "echo hi | prog"});
        CHECK_FALSE(f.last().stoppable); // the subshell's own program: it has no job control
        CHECK(f.process.foregrounds == std::vector<int32_t>{0});
        // Its status, negated as the pipeline says.
        CHECK(f.out("! echo hi | prog", 1) == "ran\n");
        // It stops as a whole.
        f.launcher.stopNext = true;
        CHECK(f.out("echo a | prog", 148) == "ran\n^Z\n[1]+  Stopped                 echo a | prog\n");
        // Ending in the shell's own code it runs here (lastpipe), and can't stop.
        size_t calls = f.launcher.calls.size();
        CHECK(f.out("echo hi | read x; echo $x", 0) == "hi\n");
        CHECK(f.launcher.calls.size() == calls);
        // So does one with a here-document whose body is outside its text.
        CHECK(f.out("wc -l <<EOF | prog\na\nb\nEOF", 0) == "ran\n");
        CHECK_FALSE(f.last().subshell);
        CHECK(f.last().ignoreStops);
}

TEST_CASE("Stopping: command substitutions can't stop; stopped jobs are reported") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        f.launcher.stopNext = true;
        CHECK(f.out("x=$(prog); echo $x", 0) == "ran\n");
        CHECK_FALSE(f.last().stoppable);
        CHECK(f.last().ignoreStops);
        f.launcher.stopNext = false;
        // A background job stopped by a read of the terminal is reported before the prompt.
        CHECK(f.out("prog &", 0) == "[1] 100\n");
        f.process.children[100].change = (21 << 8) | 0x7f;
        f.shell->start();
        CHECK(f.con.take().find("[1]+  Stopped(SIGTTIN)        prog\n") != std::string::npos);
        f.shell->start();
        CHECK(f.con.take().find("Stopped") == std::string::npos); // once
}

TEST_CASE("Stopping: exit with stopped jobs warns once") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        f.launcher.stopNext = true;
        CHECK(f.out("prog", 148) == "^Z\n[1]+  Stopped                 prog\n");
        CHECK(f.out("exit", 148) == "There are stopped jobs.\n");
        CHECK(f.out("echo hi", 0) == "hi\n");
        CHECK(f.out("exit", 0) == "There are stopped jobs.\n");
        CHECK(f.out("exit", 0) == "");
}

TEST_CASE("Hangup: the shell's jobs are hung up, but not disowned ones") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        CHECK(f.out("prog &", 0) == "[1] 100\n");
        CHECK(f.out("prog &", 0) == "[2] 101\n");
        CHECK(f.out("prog &", 0) == "[3] 102\n");
        f.launcher.stopNext = true;
        CHECK(f.out("prog", 148) == "^Z\n[4]+  Stopped                 prog\n");
        CHECK(f.out("disown -h %1", 0) == "");
        CHECK(f.out("disown 2", 0) == "");
        CHECK(f.out("jobs", 0) == "[1]   Running                 prog &\n[3]-  Running                 prog &\n"
                                  "[4]+  Stopped                 prog\n");
        CHECK(f.out("disown -x", Shell::ExitUsage) == "usage: disown\n");
        f.shell->state().jobs.hangUp();
        // SIGHUP to job 3's group; SIGHUP then SIGCONT to job 4's (stopped).
        CHECK(f.process.signals == std::vector<std::pair<int32_t, int>>{{-102, 1}, {-103, 1}, {-103, 18}});
}

TEST_CASE("Stopping: disowning a stopped job warns") {
        Fixture f;
        f.program("prog");
        f.beInteractive();
        f.launcher.stopNext = true;
        CHECK(f.out("prog", 148) == "^Z\n[1]+  Stopped                 prog\n");
        CHECK(f.out("disown", 0) == "disown: warning: deleting stopped job 1 with process group 100\n");
        CHECK(f.process.disowned == std::vector<int32_t>{100});
        CHECK(f.out("jobs", 0) == "");
}
