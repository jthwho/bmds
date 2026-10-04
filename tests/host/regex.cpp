/**
 * @file      regex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/regex.h>

using bmds::Error;
namespace Regex = bmds::Regex;

namespace {

        bool m(const char *pattern, const char *text, bool ignoreCase = false) {
                REQUIRE(Regex::check(pattern).isOk());
                return Regex::search(pattern, text, ignoreCase);
        }

} // namespace

TEST_CASE("Regex: matching") {
        CHECK(m("", "anything"));
        CHECK(m("abc", "xxabcxx"));
        CHECK_FALSE(m("abd", "abc"));
        CHECK(m("^ab", "abc"));
        CHECK_FALSE(m("^bc", "abc"));
        CHECK(m("bc$", "abc"));
        CHECK_FALSE(m("ab$", "abc"));
        CHECK(m("^$", ""));
        CHECK(m("a.c", "abc"));
        CHECK_FALSE(m("a.c", "ac"));
        CHECK(m("ab*c", "ac"));
        CHECK(m("ab*c", "abbbc"));
        CHECK(m("^a.*z$", "a to z"));
        CHECK(m("x*", ""));
        CHECK(m("[abc]x", "bx"));
        CHECK_FALSE(m("[abc]x", "dx"));
        CHECK(m("[a-c0-9]", "7"));
        CHECK(m("[^a-c]", "abcd"));
        CHECK_FALSE(m("^[^a-c]*$", "abc"));
        CHECK(m("[]x]", "]"));
        CHECK(m("[^]x]", "a"));
        CHECK(m("a\\.b", "a.b"));
        CHECK_FALSE(m("a\\.b", "axb"));
        CHECK(m("\\*", "*"));
        CHECK(m("*a", "*a"));   // a leading * is literal
        CHECK(m("a^b", "a^b")); // ^ not at the start is literal
        CHECK(m("a$b", "a$b")); // $ not at the end is literal
        CHECK(m("HeLLo", "hello", true));
        CHECK(m("[a-c]x", "BX", true));
        CHECK_FALSE(m("[a-c]x", "BX"));
}

TEST_CASE("Regex: check") {
        CHECK(Regex::check("a[b").isError());
        CHECK(Regex::check("abc\\") == Error::Invalid);
        CHECK(Regex::check("a\\(b\\)") == Error::NotSupported);
        CHECK(Regex::check("a\\{2\\}") == Error::NotSupported);
        CHECK(Regex::check("\\1") == Error::NotSupported);
        CHECK(Regex::check("\\w") == Error::NotSupported);
        CHECK(Regex::check("[[:alpha:]]") == Error::NotSupported);
        CHECK(Regex::check("a\\.b\\*\\[\\]\\^\\$\\\\\\/").isOk());
}
