/**
 * @file      parse.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <string>
#include <vector>
#include <bmds/pipeline.h>

using bmds::Error;
using bmds::ParseError;
using bmds::Pipeline;
using bmds::Redirect;

namespace {

        using Words = std::vector<std::string>;

        struct Parsed {
                        std::string        buf; // the pipeline points into this
                        Pipeline           pipeline;
                        ParseError         error;
                        Error              err;
                        std::vector<Words> words;
        };

        // Parses in place: the pipeline points into `p.buf`, so a Parsed
        // must not be copied or moved afterwards.
        void parseInto(Parsed &p, const char *line) {
                p.buf = line;
                p.err = bmds::parsePipeline(p.buf.data(), p.pipeline, p.error);
                for (const bmds::Stage &s : p.pipeline.stages) {
                        Words w;
                        for (const char *a : s.args) w.emplace_back(a);
                        p.words.push_back(w);
                }
                return;
        }

        // For results that only use `err`, `error` and `words` (no pointers).
        Parsed parse(const char *line) {
                Parsed p;
                parseInto(p, line);
                p.pipeline.stages.clear();
                return p;
        }

        // The words of a single-stage line.
        Words words(const char *line) {
                Parsed p = parse(line);
                REQUIRE(p.err.isOk());
                return p.words.empty() ? Words{} : p.words[0];
        }

} // namespace

TEST_CASE("parsePipeline: words") {
        SUBCASE("empty and blank lines") {
                CHECK(parse("").words.empty());
                CHECK(parse("   \t ").words.empty());
        }

        SUBCASE("whitespace separates") {
                CHECK(words("log level  info") == Words{"log", "level", "info"});
                CHECK(words("  a\tb  ") == Words{"a", "b"});
        }

        SUBCASE("double quotes with escapes") {
                CHECK(words(R"(echo "hello world")") == Words{"echo", "hello world"});
                CHECK(words(R"("a \"q\" \\ \n")") == Words{R"(a "q" \ \n)"});
                CHECK(words(R"("")") == Words{""});
        }

        SUBCASE("single quotes are literal") {
                CHECK(words(R"('a "b" \c')") == Words{R"(a "b" \c)"});
        }

        SUBCASE("quotes join adjacent text") {
                CHECK(words(R"(a"b c"d 'e'f)") == Words{"ab cd", "ef"});
        }

        SUBCASE("backslash outside quotes") {
                CHECK(words(R"(a\ b c\\d e\)") == Words{"a b", R"(c\d)", R"(e\)"});
        }

        SUBCASE("comments") {
                CHECK(parse("# just a comment").words.empty());
                CHECK(words("mem # show memory") == Words{"mem"});
                CHECK(words("a#b") == Words{"a#b"});
                CHECK(words(R"("#x")") == Words{"#x"});
        }

        SUBCASE("quoted operators are words") {
                CHECK(words(R"(echo "a|b" 'c>d' e\|f \>)") == Words{"echo", "a|b", "c>d", "e|f", ">"});
        }

        SUBCASE("unterminated quotes") {
                CHECK(parse(R"(echo "abc)").err == Error::Invalid);
                CHECK(parse("echo 'abc").err == Error::Invalid);
        }

        SUBCASE("many words") {
                CHECK(words("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20").size() == 20);
        }
}

TEST_CASE("parsePipeline: pipes") {
        Parsed p;
        parseInto(p, "a 1|b  |  c 2 3");
        REQUIRE(p.err.isOk());
        CHECK(p.words == std::vector<Words>{{"a", "1"}, {"b"}, {"c", "2", "3"}});

        Parsed both;
        parseInto(both, "a |& b");
        REQUIRE(both.err.isOk());
        REQUIRE(both.pipeline.stages[0].redirects.size() == 1);
        const Redirect &r = both.pipeline.stages[0].redirects[0];
        CHECK(r.kind == Redirect::Kind::Duplicate);
        CHECK(r.fd == 2);
        CHECK(r.sourceFd == 1);

        CHECK(parse("| a").error.token == std::string("|"));
        CHECK(parse("a |").error.token == std::string("newline"));
        CHECK(parse("a | | b").err == Error::Invalid);
        CHECK(parse("a || b").error.token == std::string("||"));
}

TEST_CASE("parsePipeline: redirections") {
        auto redirects = [](const char *line) {
                static Parsed p;
                p = Parsed();
                parseInto(p, line);
                REQUIRE(p.err.isOk());
                return &p;
        };

        SUBCASE("files") {
                Parsed     *p = redirects("cmd <in >out 2>>log x");
                const auto &r = p->pipeline.stages[0].redirects;
                CHECK(p->words[0] == Words{"cmd", "x"});
                REQUIRE(r.size() == 3);
                CHECK(r[0].kind == Redirect::Kind::Input);
                CHECK(r[0].fd == 0);
                CHECK(r[0].path == std::string("in"));
                CHECK(r[1].kind == Redirect::Kind::Output);
                CHECK(r[1].fd == 1);
                CHECK(r[2].kind == Redirect::Kind::Append);
                CHECK(r[2].fd == 2);
                CHECK(r[2].path == std::string("log"));
        }

        SUBCASE("a number is a descriptor only right before the operator") {
                Parsed *p = redirects("echo 2 >x 3>y");
                CHECK(p->words[0] == Words{"echo", "2"});
                const auto &r = p->pipeline.stages[0].redirects;
                REQUIRE(r.size() == 2);
                CHECK(r[0].fd == 1);
                CHECK(r[1].fd == 3);
                CHECK(redirects("echo a2>x")->words[0] == Words{"echo", "a2"});
                CHECK(redirects("echo '2'>x")->words[0] == Words{"echo", "2"});
        }

        SUBCASE("duplicate and close") {
                const auto &r = redirects("cmd 2>&1 >&- <&3 4<&-")->pipeline.stages[0].redirects;
                REQUIRE(r.size() == 4);
                CHECK(r[0].kind == Redirect::Kind::Duplicate);
                CHECK((r[0].fd == 2 && r[0].sourceFd == 1));
                CHECK(r[1].kind == Redirect::Kind::Close);
                CHECK(r[1].fd == 1);
                CHECK((r[2].kind == Redirect::Kind::Duplicate && r[2].fd == 0 && r[2].sourceFd == 3));
                CHECK((r[3].kind == Redirect::Kind::Close && r[3].fd == 4));
        }

        SUBCASE("both stdout and stderr") {
                for (const char *line : {"cmd &>f", "cmd >&f", "cmd &>>f"}) {
                        const auto &r = redirects(line)->pipeline.stages[0].redirects;
                        REQUIRE(r.size() == 2);
                        CHECK(r[0].fd == 1);
                        CHECK(r[0].path == std::string("f"));
                        CHECK((r[1].kind == Redirect::Kind::Duplicate && r[1].fd == 2 && r[1].sourceFd == 1));
                }
        }

        SUBCASE("no space needed, quoted targets") {
                const auto &r = redirects("cmd>'a b'")->pipeline.stages[0].redirects;
                REQUIRE(r.size() == 1);
                CHECK(r[0].path == std::string("a b"));
        }

        SUBCASE("errors") {
                CHECK(parse("cmd >").error.token == std::string("newline"));
                CHECK(parse("cmd > > x").error.token == std::string(">"));
                CHECK(parse("cmd <&x").error.message == std::string("ambiguous redirect"));
                CHECK(parse("a ; b").err == Error::Invalid);
                CHECK(parse("a && b").err == Error::Invalid);
                CHECK(parse("a & b").err == Error::Invalid);
                CHECK(parse("cat << EOF").err == Error::Invalid);
        }
}
