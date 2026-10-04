/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>
#include <bmds/shell.h>
#include "fakeconsole.h"

using bmds::Shell;
using bmds::Span;

namespace {

        std::vector<std::string> lastArgs;

        int runEcho(Shell &shell, Span<char *const> args) {
                lastArgs.assign(args.begin(), args.end());
                for (size_t i = 1; i < args.size(); ++i) shell.printf("%s%s", i > 1 ? " " : "", args[i]);
                shell.print("\n");
                return Shell::ExitSuccess;
        }

        int runFail(Shell &, Span<char *const>) {
                return 42;
        }

        int runStrict(Shell &shell, Span<char *const> args) {
                if (args.size() != 1) return shell.usage();
                return Shell::ExitSuccess;
        }

        // Stands in for Ctrl-C arriving while the command runs.
        int runInterrupted(Shell &shell, Span<char *const>) {
                shell.print("working");
                shell.console().interrupt().raise();
                return shell.isInterrupted() ? Shell::ExitInterrupted : Shell::ExitSuccess;
        }

        constexpr bmds::Command Commands[] = {
                {"echo", "[text]...", "Print the arguments", runEcho},
                {"fail", "", "Exit with 42", runFail},
                {"help", "[command]", "List commands", Shell::runHelp},
                {"strict", "", "Takes no arguments", runStrict},
        };

        constexpr bmds::Command InterruptCommands[] = {
                {"echo", "[text]...", "Print the arguments", runEcho},
                {"interrupted", "", "Gets interrupted", runInterrupted},
        };

        int run(Shell &shell, const char *line) {
                std::string buf(line);
                return shell.execute(buf.data());
        }

} // namespace

TEST_CASE("Shell") {
        FakeConsole con;
        auto        shell = std::make_unique<Shell>(con, Span<const bmds::Command>(Commands));

        SUBCASE("finds commands by name") {
                CHECK(shell->findCommand("echo") == &Commands[0]);
                CHECK(shell->findCommand("strict") == &Commands[3]);
                CHECK(shell->findCommand("help") == &Commands[2]);
                CHECK(shell->findCommand("nope") == nullptr);
                CHECK(shell->findCommand("") == nullptr);
                CHECK(shell->findCommand("echoo") == nullptr);
        }

        SUBCASE("runs commands with parsed arguments") {
                CHECK(run(*shell, "echo  hello \"big world\"") == Shell::ExitSuccess);
                CHECK(lastArgs == std::vector<std::string>{"echo", "hello", "big world"});
                CHECK(con.take() == "hello big world\n");
        }

        SUBCASE("exit codes") {
                CHECK(run(*shell, "fail") == 42);
                CHECK(shell->lastExitCode() == 42);
                CHECK(run(*shell, "   ") == 42); // an empty line keeps the last code
                CHECK(run(*shell, "missing arg") == Shell::ExitNotFound);
                CHECK(con.take() == "missing: command not found\n");
                CHECK(run(*shell, "echo \"open") == Shell::ExitUsage);
                CHECK(con.take() == "unterminated quote\n");
                CHECK(run(*shell, "echo 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16") == Shell::ExitUsage);
                CHECK(con.take() == "too many arguments (at most 16)\n");
        }

        SUBCASE("usage") {
                CHECK(run(*shell, "strict x") == Shell::ExitUsage);
                CHECK(con.take() == "usage: strict\n");
        }

        SUBCASE("help") {
                CHECK(run(*shell, "help") == Shell::ExitSuccess);
                CHECK(con.take() == "echo    Print the arguments\n"
                                    "fail    Exit with 42\n"
                                    "help    List commands\n"
                                    "strict  Takes no arguments\n");
                CHECK(run(*shell, "help echo") == Shell::ExitSuccess);
                CHECK(con.take() == "usage: echo [text]...\nPrint the arguments\n");
                CHECK(run(*shell, "help nope") == Shell::ExitFailure);
                CHECK(con.take() == "help: no such command: nope\n");
                CHECK(run(*shell, "help a b") == Shell::ExitUsage);
                CHECK(con.take() == "usage: help [command]\n");
        }

        SUBCASE("interactive: prompt, edit, run, prompt") {
                shell->start();
                CHECK(con.take() == "bmds> ");
                for (char c : std::string("echo hi\r")) shell->feed(c);
                CHECK(con.take() == "echo hi\nhi\nbmds> ");
                for (char c : std::string("x\x03")) shell->feed(c);
                CHECK(con.take() == "x^C\nbmds> ");
        }
}

TEST_CASE("Shell: Ctrl-C") {
        FakeConsole con;
        auto        shell = std::make_unique<Shell>(con, Span<const bmds::Command>(InterruptCommands));

        SUBCASE("an interrupted command exits 130 and the shell echoes ^C") {
                CHECK(run(*shell, "interrupted") == Shell::ExitInterrupted);
                CHECK(con.take() == "working^C\n");
                CHECK_FALSE(con.interrupt().isRaised());
        }

        SUBCASE("a Ctrl-C typed after the line still interrupts the command") {
                con.interrupt().raise();
                CHECK(run(*shell, "echo hi") == Shell::ExitInterrupted);
                CHECK(con.take() == "hi\n^C\n");
                CHECK(run(*shell, "echo hi") == Shell::ExitSuccess);
        }
}
