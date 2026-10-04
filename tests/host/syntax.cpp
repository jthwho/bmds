/**
 * @file      syntax.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Runs the parser over corpus/syntax.txt (format described there).
 */

#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <bmds/parser.h>
#include "hostmemory.h"

using bmds::Arena;
using bmds::Error;
using bmds::Parser;

namespace {

        struct Entry {
                        std::string              name;
                        std::string              input;
                        std::string              outcome; // ok, incomplete, error, unsupported
                        std::vector<std::string> expected;
                        int                      line = 0;
                        std::string              message;
        };

        std::vector<Entry> loadCorpus() {
                std::ifstream in(BMDS_CORPUS_DIR "/syntax.txt");
                REQUIRE(in.good());
                std::vector<Entry> entries;
                std::string        line;
                enum {
                        Outside,
                        Input,
                        Expected
                } state = Outside;
                while (std::getline(in, line)) {
                        if (line.rfind("=== ", 0) == 0) {
                                entries.push_back(Entry());
                                entries.back().name = line.substr(4);
                                state = Input;
                        } else if (state == Input && line.rfind("--- ", 0) == 0) {
                                Entry      &e = entries.back();
                                std::string rest = line.substr(4);
                                const auto  space = rest.find(' ');
                                e.outcome = rest.substr(0, space);
                                if (e.outcome == "error" || e.outcome == "unsupported") {
                                        const auto colon = rest.find(':');
                                        e.line = std::stoi(rest.substr(space + 1, colon - space - 1));
                                        e.message = rest.substr(colon + 2);
                                }
                                state = Expected;
                        } else if (state == Input) {
                                entries.back().input += line + "\n";
                        } else if (state == Expected && !line.empty()) {
                                // A dump may span lines (here-document bodies): a line not
                                // starting with '(' continues the previous one.
                                Entry &e = entries.back();
                                if (line[0] == '(' || e.expected.empty()) {
                                        e.expected.push_back(line);
                                } else {
                                        e.expected.back() += "\n" + line;
                                }
                        }
                }
                return entries;
        }

} // namespace

TEST_CASE("Parser: syntax corpus") {
        const std::vector<Entry> corpus = loadCorpus();
        CHECK(corpus.size() > 80);
        const size_t base = HostMemory::liveAllocations();
        for (const Entry &e : corpus) {
                CAPTURE(e.name);
                CAPTURE(e.input);
                Arena                    arena(bmds::MemSpace::Psram, 4096);
                Parser                   parser(bmds::StringView(e.input.data(), e.input.size()), arena);
                std::vector<std::string> dumps;
                Error                    err;
                for (;;) {
                        auto [node, nextErr] = parser.next();
                        err = nextErr;
                        if (err.isError() || node == nullptr) break;
                        bmds::String out;
                        REQUIRE(bmds::dumpSyntax(node, out).isOk());
                        dumps.emplace_back(out.cStr(), out.size());
                }
                std::string actual;
                for (const std::string &d : dumps) actual += d + "\n";
                CAPTURE(actual);
                if (e.outcome == "ok") {
                        CHECK(err == Error::Ok);
                        CHECK(dumps == e.expected);
                } else if (e.outcome == "incomplete") {
                        CHECK(err == Error::NotReady);
                } else {
                        CHECK(err == Error::Invalid);
                        bmds::String message;
                        REQUIRE(bmds::appendSyntaxError(parser.error(), message).isOk());
                        CHECK(std::string(message.cStr()) == e.message);
                        CHECK(parser.error().line == static_cast<unsigned>(e.line));
                }
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("Parser: one command at a time") {
        Arena       arena(bmds::MemSpace::Psram, 4096);
        const char *src = "echo a\nif true; then\n";
        Parser      parser(src, arena);
        auto [first, err] = parser.next();
        REQUIRE(err.isOk());
        REQUIRE(first != nullptr);
        CHECK(parser.position() == 7); // after "echo a\n"
        auto [second, err2] = parser.next();
        CHECK(err2 == Error::NotReady); // the earlier command still ran
        CHECK(parser.position() == 7);
}

TEST_CASE("Parser: function source is kept for reparsing") {
        Arena  arena(bmds::MemSpace::Psram, 4096);
        Parser parser("f() { echo \"$1\"; } 2>/dev/null\n", arena);
        auto [node, err] = parser.next();
        REQUIRE(err.isOk());
        REQUIRE(node->kind == bmds::NodeKind::Function);
        const auto *f = static_cast<const bmds::FunctionNode *>(node);
        CHECK(f->name == "f");
        CHECK(f->source == "{ echo \"$1\"; } 2>/dev/null");
}

TEST_CASE("Parser: out of memory") {
        const char *src = "for x in a b c; do if [ $x ]; then echo \"${x:-y}\" | cat; fi; done\n";
        for (long successes = 0; successes < 3; ++successes) {
                Arena                 arena(bmds::MemSpace::Psram, 256);
                Parser                parser(src, arena);
                HostMemory::FailGuard fail(successes);
                auto [node, err] = parser.next();
                CHECK(err == Error::NoMem);
        }
}

TEST_CASE("Parser: every prefix and random noise parse without crashing") {
        // Truncated input is the normal interactive case; noise finds bounds bugs
        // (best run under the sanitizers).
        auto parseAll = [](const std::string &src) {
                Arena  arena(bmds::MemSpace::Psram, 1024);
                Parser parser(bmds::StringView(src.data(), src.size()), arena);
                for (int i = 0; i < 1000; ++i) {
                        auto [node, err] = parser.next();
                        if (err.isError()) {
                                CHECK((err == Error::NotReady || err == Error::Invalid));
                                return;
                        }
                        if (node == nullptr) return;
                        bmds::String out;
                        CHECK(bmds::dumpSyntax(node, out).isOk());
                }
                FAIL("parser made no progress");
        };
        for (const Entry &e : loadCorpus()) {
                for (size_t n = 0; n <= e.input.size(); ++n) parseAll(e.input.substr(0, n));
        }
        const char  *pieces[] = {"a",  " ",  "\n",   "'", "\"", "\\",   "$",   "${",  "}",    "$(", "$((",  "(",  ")",
                                 "`",  "|",  "&",    ";", "<",  ">",    "<<",  "EOF", "#",    "if", "then", "fi", "case",
                                 "in", ";;", "esac", "{", "do", "done", "for", "x=",  "2>&1", "\t", "!",    ":-", "%%"};
        unsigned int seed = 12345;
        for (int round = 0; round < 3000; ++round) {
                std::string src;
                const int   count = 1 + static_cast<int>(seed % 24);
                for (int k = 0; k < count; ++k) {
                        seed = seed * 1103515245u + 12345u;
                        src += pieces[(seed >> 16) % (sizeof(pieces) / sizeof(pieces[0]))];
                }
                parseAll(src);
        }
}
