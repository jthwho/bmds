/**
 * @file      interactive.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Interactive features: set -x, prompts, Tab completion, read at the console.
 */

#include <doctest.h>

#include <memory>
#include <string>
#include <bmds/langbuiltins.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include "fakeconsole.h"

using bmds::Shell;

namespace {

        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command Commands[] = {
                {"echo", "", "", T::echo},        {"eval", "", "", L::eval}, {"exit", "", "", L::exitCmd},
                {"export", "", "", L::exportCmd}, {"read", "", "", L::read}, {"set", "", "", L::set},
                {"true", "", "", L::colon},
        };

        struct Fixture {
                        Fixture()
                            : shell(std::make_unique<Shell>(con, bmds::Span<const bmds::Command>(Commands), 1 << 20)) {
                                REQUIRE(shell->init().isOk());
                        }

                        std::string run(const char *line) {
                                (void)shell->execute(line);
                                return con.take();
                        }

                        std::string type(const char *keys) {
                                for (const char *k = keys; *k != '\0'; ++k) shell->feed(*k);
                                return con.take();
                        }

                        std::string prompt(const char *ps) {
                                bmds::String out;
                                shell->renderPrompt(ps, out);
                                return std::string(out.cStr(), out.size());
                        }

                        FakeConsole            con;
                        std::unique_ptr<Shell> shell;
        };

} // namespace

TEST_CASE("set -x traces commands to stderr") {
        Fixture f;
        CHECK(f.run("set -x; x=1; echo \"a b\" $x; set +x") == "+ x=1\n+ echo 'a b' 1\na b 1\n+ set +x\n");
        CHECK(f.run("PS4='>> '; set -x; true; set +x") == ">> true\n>> set +x\n");
}

TEST_CASE("prompts") {
        Fixture f;
        CHECK(f.run("USER=me HOSTNAME=box PWD=/sd/work") == "");
        (void)f.shell->execute("export USER=me HOSTNAME=box PWD=/sd/work");
        CHECK(f.prompt("\\u@\\h:\\w \\W\\$ ") == "me@box:/sd/work work$ ");
        // $HOME and below show as ~, as in bash (not a mere prefix of a name).
        (void)f.shell->execute("HOME=/sd");
        CHECK(f.prompt("\\w") == "~/work");
        (void)f.shell->execute("PWD=/sd");
        CHECK(f.prompt("\\w") == "~");
        (void)f.shell->execute("PWD=/sdx");
        CHECK(f.prompt("\\w") == "/sdx");
        (void)f.shell->execute("PWD=/sd/work");
        CHECK(f.prompt("\\[\\e[1m\\]bold\\[\\e[0m\\]> ") == "\x1b[1mbold\x1b[0m> ");
        CHECK(f.prompt("$(echo sub) ${USER} \\\\ \\101\\n") == "sub me \\ A\n");
        // A prompt whose expansion fails is shown as is, and leaves no flow behind.
        CHECK(f.prompt("${nope:?x}> ") == "${nope:?x}> ");
        CHECK(f.con.take() == "nope: x\n"); // the error is reported, as bash does
        CHECK(f.run("echo still works") == "still works\n");
        // The prompt the editor shows comes from PS1.
        (void)f.shell->execute("PS1='[\\s]\\$ '");
        f.shell->start();
        CHECK(f.con.take() == "[bmds]$ ");
}

TEST_CASE("Tab completion") {
        Fixture f;
        f.shell->start();
        (void)f.con.take();
        SUBCASE("a unique command") {
                CHECK(f.type("rea\t") == "read ");
                CHECK(f.shell->isContinuing() == false);
        }
        SUBCASE("ambiguous with nothing to add: a listing") {
                CHECK(f.type("ex\t") == "ex\nexit    export  \nbmds> ex");
                CHECK(f.type("p\t") == "port "); // now unique
        }
        SUBCASE("functions and keywords where a command starts, not in arguments") {
                (void)f.shell->execute("myfunction() { :; }");
                (void)f.con.take();
                CHECK(f.type("myf\t") == "myfunction ");
                f.type("\x03");
                (void)f.con.take();
                CHECK(f.type("echo myf\t") == "echo myf\a");
                f.type("\x03");
                (void)f.con.take();
                CHECK(f.type("true; whi\t") == "true; while ");
        }
        SUBCASE("variables") {
                (void)f.shell->execute("unique_variable=1");
                CHECK(f.type("echo $unique_v\t") == "echo $unique_variable ");
                f.type("\x03");
                (void)f.con.take();
                CHECK(f.type("echo ${unique_v\t") == "echo ${unique_variable} ");
        }
}

TEST_CASE("read at the console uses a line editor") {
        Fixture f;
        f.con.input = "typed  text here\r";
        CHECK(f.run("read -p 'name? ' a b; echo \"[$a][$b]\"") == "name? typed  text here\n[typed][text here]\n");
        f.con.input += "\x04"; // Ctrl-D on an empty line: end of input
        CHECK(f.run("read x; echo $?") == "1\n");
}

TEST_CASE("a line past the editor's limit is refused, not run cut short") {
        Fixture           f;
        const std::string longWord(bmds::LineEditor::LineMax, 'a');
        f.shell->start();
        (void)f.con.take();
        // Pasted: the bytes past the limit are refused with a bell, and Enter runs nothing.
        const std::string shown = f.type(("echo " + longWord + "; echo ran\r").c_str());
        CHECK(shown.find("\nline too long (2047 bytes at most): not run\n") != std::string::npos);
        CHECK(shown.find("ran") == std::string::npos);
        CHECK(f.type("echo next\r") == "echo next\nnext\nbmds> "); // the next line runs
        // read at the console: the same.
        f.con.input = "echo " + longWord + "\r";
        const std::string read = f.run("read x; echo $? \"${#x}\"");
        const std::string tail = "\a\nread: line too long (2047 bytes at most)\n1 0\n";
        REQUIRE(read.size() > tail.size());
        CHECK(read.substr(read.size() - tail.size()) == tail);
}
