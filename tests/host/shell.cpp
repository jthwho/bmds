/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include "fakeconsole.h"

using bmds::CommandContext;
using bmds::Shell;
using bmds::Span;

namespace {

        std::vector<std::string> lastArgs;

        int runArgs(CommandContext &ctx) {
                lastArgs.assign(ctx.args().begin(), ctx.args().end());
                for (size_t i = 1; i < ctx.args().size(); ++i) ctx.printf("%s%s", i > 1 ? " " : "", ctx.args()[i]);
                ctx.print("\n");
                return Shell::ExitSuccess;
        }

        int runFail(CommandContext &) {
                return 42;
        }

        int runStrict(CommandContext &ctx) {
                if (ctx.args().size() != 1) return ctx.usage();
                return Shell::ExitSuccess;
        }

        // Stands in for Ctrl-C arriving while the command runs.
        int runInterrupted(CommandContext &ctx) {
                ctx.print("working");
                ctx.shell().console().interrupt().raise();
                return ctx.isInterrupted() ? Shell::ExitInterrupted : Shell::ExitSuccess;
        }

        int runNoisy(CommandContext &ctx) {
                ctx.print("out\n");
                ctx.eprint("err\n");
                return Shell::ExitSuccess;
        }

        // Prints forever, until interrupted or its reader goes away.
        int runYes(CommandContext &ctx) {
                while (!ctx.isInterrupted()) ctx.print("y\n");
                return Shell::ExitSuccess;
        }

        constexpr bmds::Command Commands[] = {
                {"args", "[text]...", "Print the arguments", runArgs},
                {"cat", "[-]", "cat", bmds::TextCommands::cat},
                {"echo", "[-neE] [word]...", "echo", bmds::TextCommands::echo},
                {"fail", "", "Exit with 42", runFail},
                {"grep", "[-civnqF] pattern", "grep", bmds::TextCommands::grep},
                {"head", "[-n count]", "head", bmds::TextCommands::head},
                {"help", "[command]", "List commands", Shell::runHelp},
                {"interrupted", "", "Gets interrupted", runInterrupted},
                {"noisy", "", "Writes to stdout and stderr", runNoisy},
                {"strict", "", "Takes no arguments", runStrict},
                {"wc", "[-lwc]", "wc", bmds::TextCommands::wc},
                {"yes", "", "y forever", runYes},
        };

        constexpr bmds::Command HelpCommands[] = {
                {"fail", "", "Exit with 42", runFail},
                {"help", "[command]", "List commands", Shell::runHelp},
                {"strict", "", "Takes no arguments", runStrict},
        };

        int run(Shell &shell, const char *line) {
                return shell.execute(line);
        }

        struct Fixture {
                        explicit Fixture(Span<const bmds::Command> commands = Span<const bmds::Command>(Commands))
                            : shell(std::make_unique<Shell>(con, commands, 1 << 20)) {
                                REQUIRE(shell->init().isOk());
                        }

                        // Runs @p line; returns what it printed.
                        std::string out(const char *line, int expectStatus = Shell::ExitSuccess) {
                                CHECK(run(*shell, line) == expectStatus);
                                return con.take();
                        }

                        FakeConsole            con;
                        std::unique_ptr<Shell> shell;
        };

} // namespace

TEST_CASE("Shell: commands") {
        Fixture f;
        Shell  &shell = *f.shell;

        SUBCASE("finds commands by name") {
                CHECK(shell.findCommand("args") == &Commands[0]);
                CHECK(shell.findCommand("yes") == &Commands[11]);
                CHECK(shell.findCommand("nope") == nullptr);
                CHECK(shell.findCommand("") == nullptr);
        }

        SUBCASE("runs commands with parsed arguments") {
                CHECK(f.out("args  hello \"big world\"") == "hello big world\n");
                CHECK(lastArgs == std::vector<std::string>{"args", "hello", "big world"});
        }

        SUBCASE("exit codes and errors") {
                CHECK(f.out("fail", 42) == "");
                CHECK(shell.lastExitCode() == 42);
                CHECK(f.out("   ", 42) == ""); // an empty line keeps the last code
                CHECK(f.out("missing arg", Shell::ExitNotFound) == "missing: command not found\n");
                CHECK(f.out("strict x", Shell::ExitUsage) == "usage: strict\n");
                CHECK(f.out("fi", Shell::ExitUsage) == "syntax error near unexpected token `fi'\n");
        }

        SUBCASE("an incomplete command continues on the next line") {
                CHECK(f.out("args \"open", Shell::ExitSuccess) == "");
                CHECK(shell.isContinuing());
                CHECK(f.out("quote\"") == "open\nquote\n");
                CHECK_FALSE(shell.isContinuing());
                CHECK(f.out("args a |") == "");
                CHECK(f.out("cat") == "a\n");
                CHECK(f.out("cat <<END") == "");
                CHECK(f.out("doc $((1+2))") == "");
                CHECK(f.out("END") == "doc 3\n");
        }

        SUBCASE("the prompt shows PS2 while continuing; Ctrl-C abandons the command") {
                shell.start();
                CHECK(f.con.take() == "bmds> ");
                for (char c : std::string("if true\r")) shell.feed(c);
                CHECK(f.con.take() == "if true\n> ");
                for (char c : std::string("x\x03")) shell.feed(c);
                CHECK(f.con.take() == "x^C\nbmds> ");
                CHECK_FALSE(shell.isContinuing());
        }

        SUBCASE("interactive: prompt, edit, run, prompt") {
                shell.start();
                CHECK(f.con.take() == "bmds> ");
                for (char c : std::string("echo hi\r")) shell.feed(c);
                CHECK(f.con.take() == "echo hi\nhi\nbmds> ");
                for (char c : std::string("x\x03")) shell.feed(c);
                CHECK(f.con.take() == "x^C\nbmds> ");
        }
}

TEST_CASE("Shell: help") {
        Fixture f{Span<const bmds::Command>(HelpCommands)};
        CHECK(f.out("help") == "fail    Exit with 42\n"
                               "help    List commands\n"
                               "strict  Takes no arguments\n");
        CHECK(f.out("help strict") == "usage: strict\nTakes no arguments\n");
        CHECK(f.out("help help") == "usage: help [command]\nList commands\n");
        CHECK(f.out("help nope", Shell::ExitFailure) == "help: no such command: nope\n");
        CHECK(f.out("help a b", Shell::ExitUsage) == "usage: help [command]\n");
}

TEST_CASE("Shell: pipelines") {
        Fixture f;

        CHECK(f.out("echo hello world | cat") == "hello world\n");
        CHECK(f.out("echo a b c | wc -w") == "3\n");
        CHECK(f.out("echo a b c | cat | cat | cat | wc -c") == "6\n");
        CHECK(f.out("echo -e 'one\\ntwo\\nthree' | grep t | wc -l") == "2\n");

        SUBCASE("the exit code is the last stage's") {
                CHECK(f.out("fail | cat") == "");
                CHECK(f.out("echo x | fail", 42) == "");
        }

        SUBCASE("a reader that stops early ends the writer (broken pipe)") {
                CHECK(f.out("yes | head -n 3") == "y\ny\ny\n");
                CHECK(f.out("yes | head -n 1 | cat") == "y\n");
        }

        SUBCASE("stages that can't start don't block their neighbors") {
                const std::string both = f.out("missing | echo x"); // the stages race to the console
                CHECK((both == "missing: command not found\nx\n" || both == "x\nmissing: command not found\n"));
                CHECK(f.out("echo x | missing", Shell::ExitNotFound) == "missing: command not found\n");
                CHECK(f.out("yes | missing | cat") == "missing: command not found\n");
        }

        SUBCASE("too many stages") {
                CHECK(f.out("echo | cat | cat | cat | cat | cat | cat | cat | cat", Shell::ExitFailure) ==
                      "too many pipeline stages (at most 8)\n");
                CHECK(f.out("echo x | cat | cat | cat | cat | cat | cat | cat") == "x\n");
        }

        SUBCASE("Ctrl-C stops every stage") {
                std::thread ctrlC([&f] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        f.con.interrupt().raise();
                });
                CHECK(f.out("yes | cat | grep -q nothing", Shell::ExitInterrupted) == "^C\n");
                ctrlC.join();
        }
}

TEST_CASE("Shell: redirections") {
        Fixture f;

        CHECK(f.out("noisy") == "out\nerr\n");
        CHECK(f.out("noisy > /dev/null") == "err\n");
        CHECK(f.out("noisy 2>/dev/null") == "out\n");
        CHECK(f.out("noisy >/dev/null 2>&1") == "");
        CHECK(f.out("noisy 2>&1 >/dev/null") == "err\n"); // order matters, as in bash
        CHECK(f.out("noisy &>/dev/null") == "");
        CHECK(f.out("noisy >&/dev/null") == "");
        CHECK(f.out("noisy >&-") == "err\n");
        CHECK(f.out("noisy | wc -l") == "err\n1\n");
        CHECK(f.out("noisy 2>&1 | wc -l") == "2\n");
        CHECK(f.out("noisy |& wc -l") == "2\n");
        CHECK(f.out("cat < /dev/null | wc -c") == "0\n");
        CHECK(f.out("> /dev/null") == "");

        SUBCASE("errors") {
                // No filesystem is mounted here: nothing can be created in /.
                CHECK(f.out("echo hi > out.txt", Shell::ExitFailure) == "out.txt: Permission denied\n");
                CHECK(f.out("cat < in.txt", Shell::ExitFailure) == "in.txt: No such file or directory\n");
                CHECK(f.out("echo hi 3>&5", Shell::ExitFailure) == "5: Bad file descriptor\n");
                CHECK(f.out("echo hi 99>&1", Shell::ExitFailure) == "99: Bad file descriptor\n");
                CHECK(f.out("echo hi <&x", Shell::ExitFailure) == "x: ambiguous redirect\n");
        }

        SUBCASE("syntax errors") {
                CHECK(f.out("| cat", Shell::ExitUsage) == "syntax error near unexpected token `|'\n");
                CHECK(f.out("echo >", Shell::ExitUsage) == "syntax error near unexpected token `newline'\n");
                CHECK(f.out("echo > | cat", Shell::ExitUsage) == "syntax error near unexpected token `|'\n");
                CHECK(f.out("a &", Shell::ExitUsage) == "background jobs (&) are not supported\n");
        }
}

TEST_CASE("Shell: Ctrl-C") {
        Fixture f;

        SUBCASE("an interrupted command exits 130 and the shell echoes ^C") {
                CHECK(f.out("interrupted", Shell::ExitInterrupted) == "working^C\n");
                CHECK_FALSE(f.con.interrupt().isRaised());
        }

        SUBCASE("a Ctrl-C typed after the line stops the command before it starts") {
                f.con.interrupt().raise();
                CHECK(f.out("echo hi", Shell::ExitInterrupted) == "^C\n");
                CHECK(f.out("echo hi") == "hi\n");
        }
}

TEST_CASE("Text commands") {
        Fixture f;

        SUBCASE("echo") {
                CHECK(f.out("echo") == "\n");
                CHECK(f.out("echo a  b") == "a b\n");
                CHECK(f.out("echo -n hi") == "hi");
                CHECK(f.out("echo -x hi") == "-x hi\n");
                CHECK(f.out("echo -n -e 'a\\tb'") == "a\tb");
                CHECK(f.out("echo 'a\\tb'") == "a\\tb\n");
                CHECK(f.out("echo -e 'a\\cb' c") == "a");
                CHECK(f.out("echo -e '\\x41\\0102\\e[0m\\q'") == "AB\x1b[0m\\q\n");
                CHECK(f.out("echo -eE '\\n'") == "\\n\n");
        }

        SUBCASE("cat") {
                CHECK(f.out("echo abc | cat -") == "abc\n");
                CHECK(f.out("cat file", Shell::ExitFailure) == "cat: file: No such file or directory\n");
        }

        SUBCASE("head") {
                CHECK(f.out("echo -e '1\\n2\\n3' | head -n 2") == "1\n2\n");
                CHECK(f.out("echo -e '1\\n2\\n3' | head -n2") == "1\n2\n");
                CHECK(f.out("echo -n -e '1\\n2' | head") == "1\n2\n");
                CHECK(f.out("echo x | head -n z", Shell::ExitUsage) == "usage: head [-n count]\n");
        }

        SUBCASE("wc") {
                CHECK(f.out("echo -e 'a b\\nc' | wc") == "      2       3       6\n");
                CHECK(f.out("echo -e 'a b\\nc' | wc -lc") == "      2       6\n");
                CHECK(f.out("echo -n | wc -l") == "0\n");
                CHECK(f.out("echo | wc -q", Shell::ExitUsage) == "usage: wc [-lwc]\n");
        }

        SUBCASE("grep") {
                const char *fruit = "echo -e 'apple\\nBanana\\ncherry\\ndate' | ";
                auto        g = [&](const char *rest, int status = Shell::ExitSuccess) {
                        return f.out((std::string(fruit) + rest).c_str(), status);
                };
                CHECK(g("grep an") == "Banana\n");
                CHECK(g("grep -i b") == "Banana\n");
                CHECK(g("grep -v a") == "cherry\n");
                CHECK(g("grep -c e") == "3\n");
                CHECK(g("grep -n rr") == "3:cherry\n");
                CHECK(g("grep '^[a-c]'") == "apple\ncherry\n");
                CHECK(g("grep 'e$'") == "apple\ndate\n");
                CHECK(g("grep 'p*le'") == "apple\n");
                CHECK(g("grep 'a.a'") == "Banana\n");
                CHECK(g("grep '[^a-z]'") == "Banana\n");
                CHECK(g("grep -F 'a.a'", Shell::ExitFailure) == "");
                CHECK(g("grep -iF BAN") == "Banana\n");
                CHECK(g("grep -q cherry") == "");
                CHECK(g("grep zzz", Shell::ExitFailure) == "");
                CHECK(g("grep 'a\\(b\\)'", Shell::ExitUsage) == "grep: unsupported pattern: a\\(b\\)\n");
                CHECK(g("grep '[abc'", Shell::ExitUsage) == "grep: invalid pattern: [abc\n");
                CHECK(g("grep", Shell::ExitUsage) == "usage: grep [-civnqF] pattern\n");
        }
}
