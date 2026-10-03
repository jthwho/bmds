/**
 * @file      args.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <string>
#include <vector>
#include <bmds/args.h>

using bmds::Error;

namespace {

        // Tokenizes a copy of `line`; returns the arguments, or the error.
        struct Parsed {
                        std::vector<std::string> args;
                        Error                    err;
        };

        Parsed parse(const char *line, size_t maxArgs = 16) {
                std::string         buf(line);
                std::vector<char *> argv(maxArgs);
                auto [argc, err] = bmds::tokenizeArgs(buf.data(), bmds::Span<char *>(argv.data(), argv.size()));
                Parsed p;
                p.err = err;
                if (err.isOk()) {
                        for (size_t i = 0; i < argc; ++i) p.args.emplace_back(argv[i]);
                }
                return p;
        }

        using Args = std::vector<std::string>;

} // namespace

TEST_CASE("tokenizeArgs") {
        SUBCASE("empty and blank lines") {
                CHECK(parse("").args.empty());
                CHECK(parse("   \t ").args.empty());
        }

        SUBCASE("whitespace separates") {
                CHECK(parse("log level  info").args == Args{"log", "level", "info"});
                CHECK(parse("  a\tb  ").args == Args{"a", "b"});
        }

        SUBCASE("double quotes with escapes") {
                CHECK(parse(R"(echo "hello world")").args == Args{"echo", "hello world"});
                CHECK(parse(R"("a \"q\" \\ \n")").args == Args{R"(a "q" \ \n)"});
                CHECK(parse(R"("")").args == Args{""});
        }

        SUBCASE("single quotes are literal") {
                CHECK(parse(R"('a "b" \c')").args == Args{R"(a "b" \c)"});
        }

        SUBCASE("quotes join adjacent text") {
                CHECK(parse(R"(a"b c"d 'e'f)").args == Args{"ab cd", "ef"});
        }

        SUBCASE("backslash outside quotes") {
                CHECK(parse(R"(a\ b c\\d e\)").args == Args{"a b", R"(c\d)", R"(e\)"});
        }

        SUBCASE("comments") {
                CHECK(parse("# just a comment").args.empty());
                CHECK(parse("mem # show memory").args == Args{"mem"});
                CHECK(parse("a#b").args == Args{"a#b"});
                CHECK(parse(R"("#x")").args == Args{"#x"});
        }

        SUBCASE("unterminated quotes") {
                CHECK(parse(R"(echo "abc)").err == Error::Invalid);
                CHECK(parse("echo 'abc").err == Error::Invalid);
        }

        SUBCASE("too many arguments") {
                CHECK(parse("a b c", 3).args == Args{"a", "b", "c"});
                CHECK(parse("a b c d", 3).err == Error::Full);
                CHECK(parse("a b c # d", 3).err == Error::Ok);
        }
}
