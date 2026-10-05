/**
 * @file      terminalscreen.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The terminal screen model: text, wrapping, scrolling, erasing, wide
 * characters, the alternate screen, replies, resizing and damage.
 */

#include <doctest.h>

#include <string>
#include <bmds/terminalscreen.h>
#include <bmds/utf8.h>

using bmds::Color;
using bmds::MemSpace;
using bmds::StringView;
using bmds::TerminalCell;
using bmds::TerminalScreen;
using bmds::TextStyle;

namespace {

        // A screen with its replies collected.
        struct Screen {
                        TerminalScreen screen{MemSpace::Psram};
                        std::string    replies;

                        Screen(uint16_t columns, uint16_t rows) {
                                REQUIRE(screen.resize(columns, rows).isOk());
                                screen.setReplySink(
                                        [](void *context, StringView reply) {
                                                static_cast<Screen *>(context)->replies.append(reply.data(),
                                                                                               reply.size());
                                        },
                                        this);
                        }

                        void write(const std::string &text) { screen.write(StringView(text.data(), text.size())); }

                        // Row @p r as UTF-8, a tail cell skipped, trailing blanks kept.
                        std::string row(uint16_t r) const {
                                std::string         out;
                                const TerminalCell *cells = screen.row(r);
                                for (uint16_t c = 0; c < screen.columns(); ++c) {
                                        if (cells[c].tail) continue;
                                        char         utf8[4];
                                        const size_t n = bmds::Utf8::encode(cells[c].codepoint, utf8);
                                        out.append(utf8, n);
                                }
                                return out;
                        }

                        // The whole screen, rows joined by '|'.
                        std::string text() const {
                                std::string out;
                                for (uint16_t r = 0; r < screen.rows(); ++r) {
                                        if (r > 0) out += '|';
                                        out += row(r);
                                }
                                return out;
                        }

                        bool cursorAt(uint16_t column, uint16_t r) const {
                                return screen.cursorColumn() == column && screen.cursorRow() == r;
                        }
        };

} // namespace

TEST_CASE("TerminalScreen: text, CR LF and wrapping") {
        Screen s(5, 3);
        CHECK(s.text() == "     |     |     ");
        s.write("ab\r\ncd");
        CHECK(s.text() == "ab   |cd   |     ");
        CHECK(s.cursorAt(2, 1));
        // The fifth character stays at the last column until the next one wraps.
        s.write("efg");
        CHECK(s.cursorAt(4, 1));
        s.write("h");
        CHECK(s.text() == "ab   |cdefg|h    ");
        CHECK(s.cursorAt(1, 2));
        // A CR at the last column cancels the wrap.
        s.write("\x1b[1;5HX\rY");
        CHECK(s.row(0) == "Yb  X");
        // Without autowrap the last column is overwritten.
        s.write("\x1b[?7l\x1b[3;1H123456");
        CHECK(s.row(2) == "12346");
        s.write("\x1b[?7h");
        // Tab stops at the last column; backspace from there moves back one.
        s.write("\x1b[2J\x1b[H\tz\bq");
        CHECK(s.row(0) == "   qz");
}

TEST_CASE("TerminalScreen: scrolling at the bottom moves rows") {
        Screen s(4, 3);
        s.write("1\r\n2\r\n3\r\n4\r\n5");
        CHECK(s.text() == "3   |4   |5   ");
        CHECK(s.cursorAt(1, 2));
        s.screen.clearDamage();
        CHECK(!s.screen.hasDamage());
        s.write("\r\n");
        CHECK(s.text() == "4   |5   |    ");
        for (uint16_t r = 0; r < 3; ++r) CHECK(s.screen.isRowDirty(r));
        // RI at the top scrolls down.
        s.write("\x1b[H\x1bM");
        CHECK(s.text() == "    |4   |5   ");
        // SU and SD.
        s.write("\x1b[2S");
        CHECK(s.text() == "5   |    |    ");
        s.write("\x1b[T");
        CHECK(s.text() == "    |5   |    ");
}

TEST_CASE("TerminalScreen: scroll region, insert and delete lines") {
        Screen s(3, 5);
        s.write("a\r\nb\r\nc\r\nd\r\ne");
        s.write("\x1b[2;4r"); // rows 2-4; the cursor goes home
        CHECK(s.cursorAt(0, 0));
        s.write("\x1b[4;1H\n"); // a line feed at the region's bottom
        CHECK(s.text() == "a  |c  |d  |   |e  ");
        s.write("\x1b[2;1H\x1b[L"); // insert a line at row 2
        CHECK(s.text() == "a  |   |c  |d  |e  ");
        s.write("\x1b[3;2H\x1b[2M"); // delete two at row 3; the cursor goes to column 0
        CHECK(s.text() == "a  |   |   |   |e  ");
        CHECK(s.cursorAt(0, 2));
        // Outside the region IL does nothing.
        s.write("\x1b[5;1H\x1b[L");
        CHECK(s.text() == "a  |   |   |   |e  ");
        // Cursor up stops at the region's top from inside it.
        s.write("\x1b[3;1H\x1b[9A");
        CHECK(s.cursorAt(0, 1));
        // Origin mode: rows count from the region.
        s.write("\x1b[?6h\x1b[2;3Hx\x1b[6n");
        CHECK(s.row(2) == "  x");
        CHECK(s.replies == "\x1b[2;3R");
        s.write("\x1b[?6l\x1b[r");
        s.write("\x1b[5;1H\n");
        CHECK(s.text() == "   |  x|   |e  |   ");
}

TEST_CASE("TerminalScreen: erasing uses the current background") {
        Screen s(4, 2);
        s.write("abcd\r\nefgh");
        s.write("\x1b[1;2H\x1b[41m\x1b[K");
        CHECK(s.row(0) == "a   ");
        CHECK(s.screen.row(0)[1].background == Color::indexed(1));
        CHECK(s.screen.row(0)[0].background == Color());
        s.write("\x1b[2;3H\x1b[1K");
        CHECK(s.row(1) == "   h");
        s.write("\x1b[0m\x1b[2;3H\x1b[X");
        CHECK(s.screen.row(1)[2].background == Color());
        s.write("\x1b[1;1Hxyz\x1b[2;1H\x1b[1J");
        CHECK(s.text() == "    |   h");
        s.write("\x1b[2J");
        CHECK(s.text() == "    |    ");
}

TEST_CASE("TerminalScreen: SGR is kept in the cells") {
        Screen s(4, 1);
        s.write("\x1b[1;4;38;2;10;20;30;48;5;200mA\x1b[0mB");
        const TerminalCell *cells = s.screen.row(0);
        CHECK(cells[0].attributes == (TextStyle::Bold | TextStyle::Underline));
        CHECK(cells[0].foreground == Color::rgb(10, 20, 30));
        CHECK(cells[0].background == Color::indexed(200));
        CHECK(cells[1].attributes == 0);
        CHECK(cells[1].foreground == Color());
}

TEST_CASE("TerminalScreen: wide characters") {
        Screen s(5, 3);
        s.write("ab\xe6\x97\xa5"
                "c"); // ab日c
        CHECK(s.row(0) == "ab日c");
        CHECK(s.screen.row(0)[2].wide);
        CHECK(s.screen.row(0)[3].tail);
        CHECK(s.cursorAt(4, 0));
        // Overwriting the tail blanks the left half.
        s.write("\x1b[1;4Hx");
        CHECK(s.row(0) == "ab xc");
        // A wide character at the last column wraps first.
        s.write("\x1b[2J\x1b[1;5H\xe6\x97\xa5");
        CHECK(s.text() == "     |日   |     ");
        CHECK(s.cursorAt(2, 1));
        s.write("\x1b[2J\x1b[H\xe6\x97\xa5\xe6\x97\xa5");
        CHECK(s.cursorAt(4, 0));
        s.write("\xe6\x97\xa5");
        CHECK(s.text() == "日日 |日   |     ");
        // Deleting a character's tail blanks the whole character.
        s.write("\x1b[1;2H\x1b[P");
        CHECK(s.row(0) == " 日  ");
        // Combining marks are dropped.
        s.write("\x1b[3;1He\xcc\x81!");
        CHECK(s.row(2) == "e!   ");
}

TEST_CASE("TerminalScreen: insert and delete characters") {
        Screen s(6, 1);
        s.write("abcdef\x1b[1;3H\x1b[2@");
        CHECK(s.row(0) == "ab  cd");
        s.write("\x1b[3P");
        CHECK(s.row(0) == "abd   ");
        s.write("\x1b[4hXY\x1b[4l");
        CHECK(s.row(0) == "abXYd ");
        s.write("\x1b[1;1H\x1b[2X");
        CHECK(s.row(0) == "  XYd ");
        // REP repeats the last character.
        s.write("\x1b[1;1Hq\x1b[3b");
        CHECK(s.row(0) == "qqqqd ");
}

TEST_CASE("TerminalScreen: the alternate screen") {
        Screen s(4, 2);
        s.write("main\r\nab");
        s.screen.clearDamage();
        s.write("\x1b[?1049h");
        CHECK(s.screen.isAlternateScreen());
        CHECK(s.text() == "    |    ");
        CHECK(s.screen.isRowDirty(0));
        s.write("\x1b[Hvi");
        s.write("\x1b[?1049l");
        CHECK(!s.screen.isAlternateScreen());
        CHECK(s.text() == "main|ab  ");
        CHECK(s.cursorAt(2, 1));
        // 47 switches without clearing or moving the cursor.
        s.write("\x1b[?47h");
        CHECK(s.text() == "vi  |    ");
        s.write("\x1b[?47l");
}

TEST_CASE("TerminalScreen: line drawing, tabs, modes") {
        Screen s(20, 2);
        s.write("\x1b(0lqk\x1b(Bq");
        CHECK(s.row(0).substr(0, 10) == "┌─┐q");
        s.write("\x0e"
                "x"
                "\x0f"
                "x"); // SO uses G1, which is still ASCII
        CHECK(s.row(0).substr(0, 12) == "┌─┐qxx");
        s.write("\x1b)0\x0e"
                "x\x0f");
        CHECK(s.row(0).substr(0, 15) == "┌─┐qxx│");

        // Tab stops every 8; set and clear.
        s.write("\x1b[2;1H\t");
        CHECK(s.cursorAt(8, 1));
        s.write("\x1b[3g\x1b[2;4H\x1bH\x1b[2;1H\t");
        CHECK(s.cursorAt(3, 1));
        s.write("\t");
        CHECK(s.cursorAt(19, 1));
        s.write("\x1b[Z");
        CHECK(s.cursorAt(3, 1));

        s.write("\x1b[?1h\x1b=\x1b[?2004h\x1b[?25l");
        CHECK(s.screen.isApplicationCursorKeys());
        CHECK(s.screen.isApplicationKeypad());
        CHECK(s.screen.isBracketedPaste());
        CHECK(!s.screen.isCursorVisible());
        s.write("\x1b[!p");
        CHECK(!s.screen.isApplicationCursorKeys());
        CHECK(s.screen.isCursorVisible());
        CHECK(s.screen.isBracketedPaste()); // DECSTR leaves it
        s.write("\x1b"
                "c");
        CHECK(!s.screen.isBracketedPaste());
        CHECK(s.cursorAt(0, 0));
        CHECK(s.row(0) == std::string(20, ' '));
}

TEST_CASE("TerminalScreen: replies") {
        Screen s(80, 24);
        s.write("\x1b[c");
        CHECK(s.replies == "\x1b[?62;22c");
        s.replies.clear();
        s.write("\x1b[5n\x1b[18t\x1b[3;7H\x1b[6n");
        CHECK(s.replies == "\x1b[0n\x1b[8;24;80t\x1b[3;7R");
}

TEST_CASE("TerminalScreen: resizing keeps the cursor's row") {
        Screen s(6, 4);
        s.write("one\r\ntwo\r\nthree\r\nfour");
        REQUIRE(s.screen.resize(4, 2).isOk());
        CHECK(s.text() == "thre|four");
        CHECK(s.cursorAt(3, 1));
        REQUIRE(s.screen.resize(5, 3).isOk());
        CHECK(s.text() == "thre |four |     ");
        CHECK(s.screen.isRowDirty(2));
        CHECK(s.screen.resize(0, 3) == bmds::Error::Invalid);
        CHECK(s.screen.resize(5, TerminalScreen::MaxSide + 1) == bmds::Error::Invalid);
        // Lines feed at the new bottom.
        s.write("\r\nx\r\ny");
        CHECK(s.text() == "four |x    |y    ");
}

TEST_CASE("TerminalScreen: split input and junk") {
        const std::string input = "\x1b[1;31mred\x1b[0m \xe6\x97\xa5\r\n\x1b[2;3Hz\x1b(0q\x1b(B\x1b[?1049h\x1b[?1049l";
        Screen            whole(10, 3);
        whole.write(input);
        Screen split(10, 3);
        for (char c : input) split.write(std::string(1, c));
        CHECK(whole.text() == split.text());
        CHECK(whole.cursorAt(split.screen.cursorColumn(), split.screen.cursorRow()));

        // Arbitrary bytes never leave the cursor off the screen.
        Screen   junk(7, 5);
        uint32_t x = 12345;
        for (int i = 0; i < 20000; ++i) {
                x = x * 1103515245 + 12345;
                std::string b(1, static_cast<char>(x >> 16));
                if ((x >> 8) % 7 == 0) b = "\x1b[";
                junk.write(b);
                REQUIRE(junk.screen.cursorColumn() < 7);
                REQUIRE(junk.screen.cursorRow() < 5);
        }
}
