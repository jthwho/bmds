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
using bmds::Regex;

namespace {

        bool m(const char *pattern, const char *text, unsigned int flags = Regex::Basic) {
                Regex re;
                REQUIRE(re.compile(pattern, flags).isOk());
                return re.search(text);
        }

} // namespace

TEST_CASE("Regex: basic") {
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
        CHECK(m("[abc]x", "bx"));
        CHECK_FALSE(m("[abc]x", "dx"));
        CHECK(m("[a-c0-9]", "7"));
        CHECK(m("[^a-c]", "abcd"));
        CHECK_FALSE(m("^[^a-c]*$", "abc"));
        CHECK(m("[]x]", "]"));
        CHECK(m("a\\.b", "a.b"));
        CHECK_FALSE(m("a\\.b", "axb"));
        CHECK(m("*a", "*a"));   // a leading * is literal
        CHECK(m("a+b", "a+b")); // + and | are literal in a basic expression
        CHECK(m("a|b", "a|b"));
        CHECK(m("\\(ab\\)\\{2\\}", "xababx"));
        CHECK_FALSE(m("^\\(ab\\)\\{2\\}$", "ab"));
        CHECK(m("\\(a\\)\\1", "aa")); // a back-reference
        CHECK(m("[[:digit:]][[:alpha:]]", "1a"));
        CHECK(m("HeLLo", "hello", Regex::IgnoreCase));
        CHECK(m("[a-c]x", "BX", Regex::IgnoreCase));
        CHECK_FALSE(m("[a-c]x", "BX"));
}

TEST_CASE("Regex: extended") {
        const unsigned int e = Regex::Extended;
        CHECK(m("cat|dog", "hotdog", e));
        CHECK_FALSE(m("^(cat|dog)$", "hotdog", e));
        CHECK(m("ab+c", "abbc", e));
        CHECK_FALSE(m("ab+c", "ac", e));
        CHECK(m("colou?r", "color", e));
        CHECK(m("^x{2,3}$", "xxx", e));
        CHECK_FALSE(m("^x{2,3}$", "xxxx", e));
        CHECK(m("(ha)+!", "hahaha!", e));
        CHECK(m("A|B", "b", e | Regex::IgnoreCase));
}

TEST_CASE("Regex: errors and reuse") {
        Regex re;
        CHECK_FALSE(re.search("anything")); // nothing compiled
        CHECK(re.compile("a[b", Regex::Basic) == Error::Invalid);
        CHECK(re.errorText()[0] != '\0');
        CHECK_FALSE(re.search("a[b"));
        CHECK(re.compile("abc\\", Regex::Basic) == Error::Invalid);
        CHECK(re.compile("a(b", Regex::Extended) == Error::Invalid);
        REQUIRE(re.compile("b", Regex::Basic).isOk());
        CHECK(re.errorText()[0] == '\0');
        CHECK(re.search("abc"));
        REQUIRE(re.compile("^z", Regex::Basic).isOk()); // the last one freed
        CHECK_FALSE(re.search("abc"));
        // A view that isn't NUL-terminated.
        const char text[] = "abcdef";
        REQUIRE(re.compile(bmds::StringView(text, 2), Regex::Basic).isOk());
        CHECK(re.search("xxabxx"));
        CHECK_FALSE(re.search("xxacxx"));
}
