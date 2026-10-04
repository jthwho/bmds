/**
 * @file      launch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The shell's side of running program files: what a ProgramLauncher is
 * given, and what its results become.
 */

#include <doctest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <bmds/langbuiltins.h>
#include <bmds/programlauncher.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include "fakeconsole.h"

using bmds::Error;
using bmds::Shell;

namespace {

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
                                calls.push_back(c);
                                // The program "writes" its path to its stdout.
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
                        };
                        std::vector<Call> calls;
                        int               status = 0;
                        Error             fail;

                private:
                        std::mutex _lock;
        };

        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command Commands[] = {
                {"echo", "", "", T::echo},
                {"export", "", "", L::exportCmd},
                {"wc", "", "", T::wc},
        };

        struct Fixture {
                        Fixture()
                            : shell(std::make_unique<Shell>(con, bmds::Span<const bmds::Command>(Commands), 1 << 20)) {
                                REQUIRE(shell->init().isOk());
                                shell->setLauncher(&launcher);
                        }
                        std::string out(const char *line, int expect) {
                                CHECK_MESSAGE(shell->execute(line) == expect, line);
                                return con.take();
                        }
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

} // namespace

TEST_CASE("Launch: a command with a / goes to the launcher") {
        Fixture f;
        f.launcher.status = 3;
        CHECK(f.out("export SHOWN=yes; HIDDEN=no; ./prog one 'two words'; echo $?", 0) == "ran\n3\n");
        REQUIRE(f.launcher.calls.size() == 1);
        const auto &c = f.launcher.calls[0];
        CHECK(c.path == "./prog");
        CHECK(c.args == std::vector<std::string>{"./prog", "one", "two words"});
        CHECK(has(c.env, "SHOWN=yes"));
        CHECK(has(c.env, "TERM=xterm-256color"));
        CHECK_FALSE(has(c.env, "HIDDEN=no"));
        CHECK(c.cwd == "/");
        // A prefix assignment reaches the program, for that command only.
        CHECK(f.out("ONCE=1 /bin/prog", 3) == "ran\n");
        CHECK(has(f.launcher.calls[1].env, "ONCE=1"));
        // Programs work in pipelines and with redirections like any command.
        f.launcher.status = 0;
        CHECK(f.out("./prog | wc -l", 0) == "1\n");
        CHECK(f.out("./prog > /dev/null", 0) == "");
        // A word without / is never a file.
        CHECK(f.out("prog", Shell::ExitNotFound) == "prog: command not found\n");
}

TEST_CASE("Launch: failures become 127 and 126") {
        Fixture f;
        f.launcher.fail = Error::NotExist;
        CHECK(f.out("./nope", Shell::ExitNotFound) == "ran\n");
        f.launcher.fail = Error::NotSupported;
        CHECK(f.out("./text.txt", Shell::ExitCannotRun) == "ran\n");
        f.launcher.fail = Error::IsDir;
        CHECK(f.out("./dir", Shell::ExitCannotRun) == "ran\n");
        // Without a launcher, a path is just a command that isn't there.
        f.shell->setLauncher(nullptr);
        CHECK(f.out("./prog", Shell::ExitNotFound) == "./prog: command not found\n");
}
