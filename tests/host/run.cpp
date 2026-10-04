/**
 * @file      run.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Runs corpus/run.txt scripts through the shell and compares stdout and the
 * exit status with what bash does (check_bash.py keeps the file honest).
 */

#include <doctest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <bmds/langbuiltins.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include "fakeconsole.h"
#include "hostmemory.h"

using bmds::Shell;

namespace {

        // A thread-safe output capture (pipeline stages write concurrently).
        class Capture final : public bmds::Stream {
                public:
                        bmds::Result<size_t> read(bmds::WritableByteSpan, unsigned int) override {
                                return bmds::makeError<size_t>(bmds::Error::EndOfFile);
                        }
                        bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int) override {
                                std::lock_guard<std::mutex> lock(_lock);
                                text.append(reinterpret_cast<const char *>(data.data()), data.size());
                                return bmds::makeResult(data.size());
                        }
                        std::string text;

                private:
                        std::mutex _lock;
        };

        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command Commands[] = {
                {":", "", "", L::colon},
                {"[", "", "", L::test},
                {"break", "", "", L::breakCmd},
                {"cat", "", "", T::cat},
                {"command", "", "", L::command},
                {"continue", "", "", L::continueCmd},
                {"echo", "", "", T::echo},
                {"eval", "", "", L::eval},
                {"exit", "", "", L::exitCmd},
                {"export", "", "", L::exportCmd},
                {"false", "", "", L::falseCmd},
                {"grep", "", "", T::grep},
                {"head", "", "", T::head},
                {"local", "", "", L::local},
                {"printf", "", "", L::printfCmd},
                {"read", "", "", L::read},
                {"readonly", "", "", L::readonly},
                {"return", "", "", L::returnCmd},
                {"set", "", "", L::set},
                {"shift", "", "", L::shift},
                {"test", "", "", L::test},
                {"true", "", "", L::colon},
                {"type", "", "", L::type},
                {"unset", "", "", L::unset},
                {"wc", "", "", T::wc},
        };

        struct Entry {
                        std::string name;
                        std::string script;
                        std::string out;
                        int         status = 0;
        };

        std::vector<Entry> loadCorpus() {
                std::ifstream in(BMDS_CORPUS_DIR "/run.txt");
                REQUIRE(in.good());
                std::vector<Entry> entries;
                std::string        line;
                enum {
                        Outside,
                        Script,
                        Out
                } state = Outside;
                while (std::getline(in, line)) {
                        if (line.rfind("=== ", 0) == 0) {
                                entries.push_back(Entry());
                                entries.back().name = line.substr(4);
                                state = Script;
                        } else if (state != Outside && line == "--- out") {
                                state = Out;
                        } else if (state == Out && line.rfind("--- status ", 0) == 0) {
                                entries.back().status = std::stoi(line.substr(11));
                                state = Outside;
                        } else if (state == Script) {
                                entries.back().script += line + "\n";
                        } else if (state == Out) {
                                entries.back().out += line + "\n";
                        }
                }
                // Blank lines at the end of an output block are the separator.
                for (Entry &e : entries) {
                        while (e.out.size() >= 2 && e.out.compare(e.out.size() - 2, 2, "\n\n") == 0) e.out.pop_back();
                        if (e.out == "\n") e.out.clear();
                }
                return entries;
        }

        // A shell whose stdout and stderr are captured.
        struct Runner {
                        Runner()
                            : shell(std::make_unique<Shell>(con, bmds::Span<const bmds::Command>(Commands), 1 << 20)) {
                                REQUIRE(shell->init().isOk());
                                auto [o, e1] = bmds::FileHandle::open(out, bmds::FileHandle::Write);
                                auto [e, e2] = bmds::FileHandle::open(err, bmds::FileHandle::Write);
                                REQUIRE((e1.isOk() && e2.isOk()));
                                (void)shell->fds().set(1, std::move(o));
                                (void)shell->fds().set(2, std::move(e));
                        }
                        ~Runner() { shell->fds().closeAll(); }

                        FakeConsole            con;
                        Capture                out;
                        Capture                err;
                        std::unique_ptr<Shell> shell;
        };

} // namespace

TEST_CASE("Executor: run corpus") {
        const std::vector<Entry> corpus = loadCorpus();
        CHECK(corpus.size() > 50);
        const size_t base = HostMemory::liveAllocations();
        for (const Entry &e : corpus) {
                CAPTURE(e.name);
                CAPTURE(e.script);
                Runner    r;
                const int status = r.shell->runScript(bmds::StringView(e.script.data(), e.script.size()));
                CAPTURE(r.err.text);
                CHECK(r.out.text == e.out);
                CHECK(status == e.status);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Executor: recursion limits are errors, not crashes") {
        Runner r;
        CHECK(r.shell->runScript("f() { f; }; f; echo after\n") != 0);
        CHECK(r.err.text.find("maximum function nesting level exceeded") != std::string::npos);
}

TEST_CASE("Executor: stack budget stops deep recursion") {
        // A small budget, as on the target: recursion through $(...) is caught
        // by the stack check before the call-depth limit.
        FakeConsole con;
        Shell       shell(con, bmds::Span<const bmds::Command>(Commands), 16384);
        REQUIRE(shell.init().isOk());
        Capture err;
        auto [e, openErr] = bmds::FileHandle::open(err, bmds::FileHandle::Write);
        REQUIRE(openErr.isOk());
        (void)shell.fds().set(2, std::move(e));
        CHECK(shell.runScript("f() { echo $(f); }; f\n") != 0);
        CHECK(err.text.find("maximum nesting depth exceeded") != std::string::npos);
        shell.fds().closeAll();
}

TEST_CASE("Executor: Ctrl-C stops a loop") {
        Runner      r;
        std::thread ctrlC([&r] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                r.con.interrupt().raise();
        });
        CHECK(r.shell->runScript("while true; do :; done; echo never\n") == Shell::ExitInterrupted);
        ctrlC.join();
        CHECK(r.out.text.empty());
}
