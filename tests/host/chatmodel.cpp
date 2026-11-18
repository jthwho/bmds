/**
 * @file      chatmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ChatView's model (devplan/chatview.md §4 step 1): IRC's codes into runs, the gutter, links,
 * wrapping in a made-up measure (8 pixels a character, bold 9), hits, the lines kept with their
 * rows.
 */

#include <doctest.h>

#include <string>
#include <vector>

#include <bmds/chatmodel.h>
#include <bmds/utf8.h>

using bmds::ChatLines;
using bmds::StringView;
namespace ChatText = bmds::ChatText;

namespace {

        // 8 pixels a character (a UTF-8 one counts once), 9 in bold.
        uint32_t charWidth(uint8_t flags) {
                return (flags & ChatText::Bold) != 0 ? 9 : 8;
        }

        uint32_t width(void *, StringView text, uint8_t flags) {
                uint32_t chars = 0;
                for (char c : text)
                        if (!bmds::Utf8::isContinuation(uint8_t(c))) ++chars;
                return chars * charWidth(flags);
        }

        size_t fitting(void *, StringView text, uint8_t flags, uint32_t most) {
                const uint32_t w = charWidth(flags);
                size_t         i = 0;
                uint32_t       used = 0;
                while (i < text.size()) {
                        size_t n = 1;
                        while (i + n < text.size() && bmds::Utf8::isContinuation(uint8_t(text[i + n]))) ++n;
                        if (used + w > most) break;
                        used += w;
                        i += n;
                }
                return i;
        }

        const ChatText::Measure Fake{width, fitting, nullptr};

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        ChatText::Parsed parsed(const char *line) {
                ChatText::Parsed p;
                REQUIRE(ChatText::parse(line, p).isOk());
                return p;
        }

        std::string runText(const ChatText::Parsed &p, size_t r) {
                return str(p.text.view().mid(p.runs[r].start, p.runs[r].length));
        }

        // Each row's text, its pieces joined, with the first piece's x.
        struct Wrapped {
                        std::vector<std::string> rows;
                        std::vector<uint32_t>    xs;
        };

        Wrapped wrapped(const ChatText::Parsed &p, uint32_t w, uint32_t textX = 0) {
                ChatText::Row    rows[32];
                ChatText::Piece  pieces[128];
                ChatText::Layout l{rows, 32, pieces, 128};
                const size_t     n = ChatText::wrap(p, Fake, w, textX, &l);
                REQUIRE(n == l.rowCount);
                REQUIRE(ChatText::wrap(p, Fake, w, textX, nullptr) == n); // counted alike without a layout
                Wrapped out;
                for (size_t r = 0; r < l.rowCount; ++r) {
                        std::string text;
                        for (uint32_t k = 0; k < rows[r].pieceCount; ++k) {
                                const ChatText::Piece &pc = pieces[rows[r].firstPiece + k];
                                text += str(p.text.view().mid(pc.start, pc.length));
                        }
                        out.rows.push_back(text);
                        out.xs.push_back(rows[r].pieceCount > 0 ? pieces[rows[r].firstPiece].x : 0);
                }
                return out;
        }

} // namespace

TEST_CASE("ChatText: styles from IRC's codes") {
        ChatText::Parsed p = parsed("plain \x02"
                                    "bold\x02 \x1ditalic\x0f end \x1fu\x1e"
                                    "s\x11m\x16r");
        CHECK(str(p.text.view()) == "plain bold italic end usmr");
        REQUIRE(p.runCount == 9);
        CHECK(runText(p, 1) == "bold");
        CHECK(p.runs[1].style.flags == ChatText::Bold);
        CHECK(p.runs[2].style.flags == 0);
        CHECK(runText(p, 3) == "italic");
        CHECK(p.runs[3].style.flags == ChatText::Italic);
        CHECK(runText(p, 4) == " end ");
        CHECK(p.runs[5].style.flags == ChatText::Underline);
        CHECK(p.runs[6].style.flags == (ChatText::Underline | ChatText::Strike));
        CHECK(p.runs[7].style.flags == (ChatText::Underline | ChatText::Strike | ChatText::Monospace));
        CHECK(p.runs[8].style.flags ==
              (ChatText::Underline | ChatText::Strike | ChatText::Monospace | ChatText::Reverse));

        // Colours: an index (one or two digits), a background, none (99), off; RGB.
        p = parsed("\x03"
                   "04red\x03 \x03"
                   "4,12onblue\x03,x \x03"
                   "99plain \x04"
                   "FF8800orange\x04 \x04"
                   "00ff00,0000ffgb");
        CHECK(str(p.text.view()) == "red onblue,x plain orange gb");
        CHECK(runText(p, 0) == "red");
        CHECK((p.runs[0].style.fg == ChatText::Colour{ChatText::Colour::Kind::Index, 4, 0}));
        CHECK(p.runs[0].style.bg.kind == ChatText::Colour::Kind::None);
        CHECK(runText(p, 2) == "onblue");
        CHECK(p.runs[2].style.bg.index == 12);
        CHECK(runText(p, 3) == ",x plain "); // a comma without a digit is text; 99 is no colour
        CHECK(runText(p, 4) == "orange");
        CHECK((p.runs[4].style.fg == ChatText::Colour{ChatText::Colour::Kind::Rgb, 0, 0xff8800}));
        CHECK(p.runs[6].style.bg.rgb == 0x0000ff);
        CHECK(ChatText::paletteColour(4) == 0xff0000);
        CHECK(ChatText::paletteColour(98) == 0xffffff);
        CHECK(ChatText::paletteColour(99) == 0);

        // Other control characters go; UTF-8 stays; codes at the end leave no empty run.
        p = parsed("a\x01"
                   "b\x07"
                   "c\x7f"
                   "d h\xc3\xa9llo \xe2\x9c\x93\x02");
        CHECK(str(p.text.view()) == "abcd h\xc3\xa9llo \xe2\x9c\x93");
        CHECK(p.runCount == 1);
        p = parsed("");
        CHECK(p.text.isEmpty());
        CHECK(p.runCount == 0);
}

TEST_CASE("ChatText: the gutter, plain text, too many runs") {
        ChatText::Parsed p = parsed("10:41 \x02<alice>\x02\thello\tworld");
        CHECK(str(p.text.view()) == "10:41 <alice>hello world");
        CHECK(p.gutter == 13);
        bmds::String plain;
        REQUIRE(ChatText::plain("10:41 \x02<alice>\x02\thello \x03"
                                "4there",
                                plain)
                        .isOk());
        CHECK(str(plain.view()) == "10:41 <alice>\thello there");
        REQUIRE(ChatText::plain("no gutter", plain).isOk());
        CHECK(str(plain.view()) == "no gutter");

        // Past MaxRuns the style stays as it was: no run lost, all the text covered.
        std::string many;
        for (int i = 0; i < 200; ++i) many += "\x02x";
        p = parsed(many.c_str());
        CHECK(p.text.size() == 200);
        CHECK(p.runCount <= ChatText::MaxRuns);
        uint32_t covered = 0;
        for (size_t r = 0; r < p.runCount; ++r) {
                CHECK(p.runs[r].start == covered);
                covered += p.runs[r].length;
        }
        CHECK(covered == 200);
}

TEST_CASE("ChatText: links") {
        ChatText::Parsed p =
                parsed("see https://example.com/a(b)c, and (www.x.org) or <irc://libera.chat/#bmds>. not xhttp://no "
                       "or https:// alone; HTTPS://UP.example!");
        auto link = [&](size_t i) {
                return str(p.text.view().mid(p.links[i].start, p.links[i].length));
        };
        REQUIRE(p.linkCount == 4);
        CHECK(link(0) == "https://example.com/a(b)c");
        CHECK(link(1) == "www.x.org");
        CHECK(link(2) == "irc://libera.chat/#bmds");
        CHECK(link(3) == "HTTPS://UP.example");
        // The runs are cut at the links and told which.
        for (size_t r = 0; r < p.runCount; ++r) {
                const ChatText::Run &run = p.runs[r];
                const int            expected = ChatText::linkAt(p, run.start);
                CHECK(run.link == expected);
                CHECK(ChatText::linkAt(p, run.start + run.length - 1) == expected);
        }
        CHECK(ChatText::linkAt(p, 0) == -1);
        CHECK(ChatText::linkAt(p, p.links[1].start) == 1);
        // A link in a style keeps the style.
        p = parsed("\x02"
                   "bold www.b.org\x02 x");
        REQUIRE(p.linkCount == 1);
        bool sawLink = false;
        for (size_t r = 0; r < p.runCount; ++r)
                if (p.runs[r].link == 0) {
                        sawLink = true;
                        CHECK(p.runs[r].style.flags == ChatText::Bold);
                        CHECK(runText(p, r) == "www.b.org");
                }
        CHECK(sawLink);
}

TEST_CASE("ChatText: wrapping") {
        // Ten characters a row: breaks at spaces, the spaces there dropped.
        Wrapped w = wrapped(parsed("hello world foo"), 80);
        REQUIRE(w.rows.size() == 2);
        CHECK(w.rows[0] == "hello ");
        CHECK(w.rows[1] == "world foo");

        // A gutter, and the text hanging past it on every row.
        w = wrapped(parsed("12:00 <a>\tsome words that wrap around"), 200, 80);
        REQUIRE(w.rows.size() == 2);
        CHECK(w.rows[0] == "12:00 <a>some words that");
        CHECK(w.rows[1] == "wrap around");
        CHECK(w.xs[0] == 0);
        CHECK(w.xs[1] == 80);

        // A word wider than a row, broken at the edge; after a gutter it starts on the gutter's row.
        w = wrapped(parsed("abcdefghijklmnopqrstuvwxyz"), 80);
        REQUIRE(w.rows.size() == 3);
        CHECK(w.rows[0] == "abcdefghij");
        CHECK(w.rows[2] == "uvwxyz");
        w = wrapped(parsed("t\tabcdefghijklmnopqrstuvwxyz"), 120, 16);
        CHECK(w.rows[0] == "tabcdefghijklm"); // 8 + 13 characters from x 16: 112
        CHECK(w.xs[1] == 16);

        // Too narrow for the indent: continuation rows from 0.
        w = wrapped(parsed("12:00\tone two three four"), 100, 80);
        CHECK(w.xs.back() == 0);

        // Bold is wider: it wraps sooner.
        CHECK(wrapped(parsed("aaaa bbbb"), 72).rows.size() == 1);
        CHECK(wrapped(parsed("\x02"
                             "aaaa bbbb"),
                      72)
                      .rows.size() == 2);

        // Empty, spaces only, leading spaces kept, a row's width of 0.
        CHECK(wrapped(parsed(""), 80).rows.size() == 1);
        CHECK(wrapped(parsed("   "), 80).rows[0] == "   ");
        CHECK(wrapped(parsed("  indented"), 200).rows[0] == "  indented");
        CHECK(wrapped(parsed("abc"), 0).rows.size() == 3);

        // UTF-8 is broken only between characters.
        w = wrapped(parsed("\xe2\x9c\x93\xe2\x9c\x93\xe2\x9c\x93"), 16);
        REQUIRE(w.rows.size() == 2);
        CHECK(w.rows[0] == "\xe2\x9c\x93\xe2\x9c\x93");
}

TEST_CASE("ChatText: a font change starts on the measure's column") {
        ChatText::Measure aligned = Fake;
        aligned.align = 4;
        ChatText::Row    rows[8];
        ChatText::Piece  pieces[16];
        ChatText::Layout l{rows, 8, pieces, 16};

        // Bold "ab" ends at 18; "cd", back in the regular font, starts at 20.
        const ChatText::Parsed p = parsed("\x02"
                                          "ab\x02"
                                          "cd ef");
        REQUIRE(ChatText::wrap(p, aligned, 60, 0, &l) == 1);
        REQUIRE(l.pieceCount == 4); // "ab" "cd" " " "ef"
        CHECK(pieces[0].x == 0);
        CHECK(pieces[1].x == 20);
        CHECK(pieces[2].x == 36);
        CHECK(pieces[3].x == 44);
        CHECK(ChatText::wrap(p, aligned, 59, 0, nullptr) == 2); // "ef" ends at 60
        REQUIRE(ChatText::wrap(p, Fake, 60, 0, &l) == 1);
        CHECK(pieces[1].x == 18); // no column asked for
        CHECK(ChatText::hit(p, aligned, l, 0, 19) == 2);

        // A colour alone keeps the font: no column.
        REQUIRE(ChatText::wrap(parsed("\x03"
                                      "04ab\x03"
                                      "cd"),
                               aligned, 60, 0, &l) == 1);
        CHECK(pieces[1].x == 16);

        // The gutter's width; text past a gutter wider than the column starts on the next one.
        const ChatText::Parsed g = parsed("\x02"
                                          "nicks\x02\tmsg");
        CHECK(ChatText::gutterWidth(g, aligned) == 45);
        CHECK(ChatText::gutterWidth(parsed("no gutter"), aligned) == 0);
        REQUIRE(ChatText::wrap(g, aligned, 200, 32, &l) == 1);
        CHECK(pieces[1].x == 48);
        REQUIRE(ChatText::wrap(g, aligned, 200, 52, &l) == 1);
        CHECK(pieces[1].x == 52);

        // A word broken at the edge counts the column too: bold "a", then "bcdefgh" from 12.
        REQUIRE(ChatText::wrap(parsed("\x02"
                                      "a\x02"
                                      "bcdefgh"),
                               aligned, 40, 0, &l) == 2);
        CHECK(pieces[1].x == 12);
        CHECK(pieces[1].length == 3); // 12 + 24 = 36; a fourth would reach 44
}

TEST_CASE("ChatText: finding text") {
        const ChatText::Parsed p = parsed("10:41\t<Alice> \x02"
                                          "ALICE\x02 said alice");
        const StringView       t = p.text.view(); // "10:41<Alice> ALICE said alice": codes gone
        CHECK(ChatText::find(t, "alice", 0, false) == 6);
        CHECK(ChatText::find(t, "alice", 7, false) == 13); // at or after: past the first
        CHECK(ChatText::find(t, "alice", 13, false) == 13);
        CHECK(ChatText::find(t, "alice", 25, false) == ChatText::NoMatch);
        CHECK(ChatText::find(t, "alice", 999, true) == 24); // past the end: from the end
        CHECK(ChatText::find(t, "alice", 24, true) == 13);  // before, not at
        CHECK(ChatText::find(t, "alice", 6, true) == ChatText::NoMatch);
        CHECK(ChatText::find(t, "Said ", 0, false) == 19);
        CHECK(ChatText::find(t, "", 0, false) == ChatText::NoMatch);
        CHECK(ChatText::find("ab", "abc", 0, false) == ChatText::NoMatch);
        CHECK(ChatText::find("ab", "ab", 2, true) == 0);
        CHECK(ChatText::find("ab", "ab", 1, false) == ChatText::NoMatch);
}

TEST_CASE("ChatText: a point's byte") {
        const ChatText::Parsed p = parsed("ab\tcdef ghij");
        ChatText::Row          rows[8];
        ChatText::Piece        pieces[16];
        ChatText::Layout       l{rows, 8, pieces, 16};
        REQUIRE(ChatText::wrap(p, Fake, 64, 24, &l) == 2); // "ab" "cdef " | "ghij" from 24
        CHECK(ChatText::hit(p, Fake, l, 0, 0) == 0);
        CHECK(ChatText::hit(p, Fake, l, 0, 11) == 1);  // nearer the second character's left
        CHECK(ChatText::hit(p, Fake, l, 0, 13) == 2);  // nearer its right
        CHECK(ChatText::hit(p, Fake, l, 0, 20) == 2);  // the gap to the text: its start
        CHECK(ChatText::hit(p, Fake, l, 0, 28) == 2);  // in 'c'
        CHECK(ChatText::hit(p, Fake, l, 0, 500) == 7); // past the row: its end
        CHECK(ChatText::hit(p, Fake, l, 1, 0) == 7);   // before a hanging row: its start
        CHECK(ChatText::hit(p, Fake, l, 1, 44) == 9);  // the middle of 'i': its left
        CHECK(ChatText::hit(p, Fake, l, 1, 45) == 10);
        CHECK(ChatText::hit(p, Fake, l, 9, 0) == 11); // no such row: the end
}

TEST_CASE("ChatLines: kept, dropped, found by row") {
        ChatLines lines(3);
        CHECK(lines.count() == 0);
        uint32_t within = 9;
        CHECK(lines.lineAtRow(0, within) == 0);
        const uint32_t rows[] = {1, 2, 3, 1, 2};
        for (int i = 0; i < 5; ++i) REQUIRE(lines.append(("line " + std::to_string(i)).c_str(), rows[i]).isOk());
        CHECK(lines.count() == 3);
        CHECK(lines.first() == 2);
        CHECK(lines.next() == 5);
        CHECK(!lines.has(1));
        CHECK(str(lines.line(2)) == "line 2");
        CHECK(lines.totalRows() == 6);
        CHECK(lines.rowOf(2) == 0);
        CHECK(lines.rowOf(4) == 4);
        CHECK(lines.lineAtRow(0, within) == 2);
        CHECK(within == 0);
        CHECK(lines.lineAtRow(2, within) == 2);
        CHECK(within == 2);
        CHECK(lines.lineAtRow(3, within) == 3);
        CHECK(lines.lineAtRow(5, within) == 4);
        CHECK(within == 1);
        CHECK(lines.lineAtRow(99, within) == 4);
        CHECK(within == 1);

        // Rewrapped: rows set again, then added up.
        lines.setRows(2, 1);
        lines.setRows(3, 1);
        lines.setRows(4, 1);
        lines.restack();
        CHECK(lines.totalRows() == 3);
        CHECK(lines.lineAtRow(1, within) == 3);

        // Fewer kept: the newest stay, in order; the numbers go on.
        REQUIRE(lines.setMaxLines(2).isOk());
        CHECK(lines.first() == 3);
        CHECK(str(lines.line(4)) == "line 4");
        REQUIRE(lines.append("line 5", 4).isOk());
        CHECK(lines.first() == 4);
        CHECK(lines.totalRows() == 5);
        lines.clear();
        CHECK(lines.count() == 0);
        CHECK(lines.next() == 6);
        CHECK(lines.totalRows() == 0);
        REQUIRE(lines.append("again", 1).isOk());
        CHECK(lines.first() == 6);
        CHECK(lines.lineAtRow(0, within) == 6);
}
