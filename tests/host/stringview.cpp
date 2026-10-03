/**
 * @file      stringview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/stringview.h>

using bmds::StringView;

TEST_CASE("StringView") {
        SUBCASE("construction") {
                CHECK(StringView().isEmpty());
                CHECK(StringView(nullptr).isEmpty());
                CHECK(StringView("abc").size() == 3);
                CHECK(StringView("abcdef", 2) == "ab");
        }

        SUBCASE("comparison") {
                CHECK(StringView("abc") == StringView("abc"));
                CHECK(StringView("abc") != StringView("abd"));
                CHECK(StringView("abc") < StringView("abd"));
                CHECK(StringView("b") > StringView("abc"));
        }

        SUBCASE("left / right / mid clamp") {
                StringView s("hello");
                CHECK(s.left(2) == "he");
                CHECK(s.left(99) == "hello");
                CHECK(s.right(3) == "llo");
                CHECK(s.right(99) == "hello");
                CHECK(s.mid(1, 3) == "ell");
                CHECK(s.mid(2) == "llo");
                CHECK(s.mid(4, 99) == "o");
                CHECK(s.mid(5).isEmpty());
                CHECK(s.mid(99).isEmpty());
        }

        SUBCASE("searching") {
                StringView s("path/to/file.txt");
                CHECK(s.startsWith("path"));
                CHECK(s.startsWith('p'));
                CHECK(s.endsWith(".txt"));
                CHECK(s.endsWith('t'));
                CHECK_FALSE(s.endsWith(".bin"));
                CHECK(s.contains("to"));
                CHECK(s.contains('.'));
                CHECK(s.indexOf('/') == 4);
                CHECK(s.indexOf('/', 5) == 7);
                CHECK(s.indexOf("file") == 8);
                CHECK(s.indexOf('#') == StringView::NPos);
                CHECK(s.lastIndexOf('/') == 7);
        }

        SUBCASE("trimmed") {
                CHECK(StringView("  a b \t\r\n").trimmed() == "a b");
                CHECK(StringView(" \t ").trimmed().isEmpty());
                CHECK(StringView("x").trimmed() == "x");
        }

        SUBCASE("iteration and access") {
                StringView s("xyz");
                CHECK(s.front() == 'x');
                CHECK(s.back() == 'z');
                CHECK(s[1] == 'y');
                int count = 0;
                for (char ch : s) {
                        (void)ch;
                        ++count;
                }
                CHECK(count == 3);
        }

        SUBCASE("constexpr") {
                constexpr StringView s("  command arg ");
                static_assert(s.trimmed().left(7) == "command");
                static_assert(s.trimmed().indexOf(' ') == 7);
        }
}
