/**
 * @file      terminalselection.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A terminal's selection (devplan/clipboard.md C10, §4d): wrapped rows,
 * units, blocks, its text and its drawing.
 */

#include <doctest.h>

#include <string>
#include <vector>
#include <bmds/base64.h>
#include <bmds/terminalscreen.h>
#include <bmds/terminalselection.h>
#include <bmds/terminalview.h>

using bmds::Error;
using bmds::StringView;
using bmds::TerminalCell;
using bmds::TerminalScreen;
using bmds::TerminalSelection;
namespace TerminalView = bmds::TerminalView;
using Unit = TerminalSelection::Unit;
using Point = TerminalSelection::Point;

namespace {

        const TerminalCell *lines(void *arg, int64_t line, uint16_t &length) {
                return TerminalView::line(*static_cast<TerminalScreen *>(arg), line, length);
        }

        Error collect(void *arg, StringView piece) {
                static_cast<std::string *>(arg)->append(piece.data(), piece.size());
                return Error::Ok;
        }

        struct Screen {
                        explicit Screen(uint16_t columns, uint16_t rows = 4) {
                                REQUIRE(screen.resize(columns, rows).isOk());
                                REQUIRE(screen.setHistory(100).isOk());
                                selection.setColumns(columns);
                        }

                        // A line of the screen shown, numbered as the selection numbers them.
                        int64_t row(uint16_t r) const { return TerminalView::lineOfViewRow(screen, 0, r); }

                        void begin(uint16_t r, uint16_t column, Unit unit = Unit::Character, bool block = false) {
                                selection.begin(Point{row(r), column}, unit, block, lines, &screen);
                        }

                        void extend(uint16_t r, uint16_t column) {
                                selection.extend(Point{row(r), column}, lines, &screen);
                        }

                        std::string text() const {
                                std::string out;
                                CHECK(selection.text(lines, const_cast<TerminalScreen *>(&screen), collect, &out) ==
                                      Error::Ok);
                                return out;
                        }

                        TerminalScreen    screen{bmds::MemSpace::Psram};
                        TerminalSelection selection;
        };

} // namespace

TEST_CASE("TerminalScreen: a wrapped row says so on its last cell, into the history") {
        Screen s(10);
        s.screen.write("0123456789abc\r\nshort");
        CHECK(s.screen.row(0)[9].wrapped == 1);
        CHECK(s.screen.row(1)[9].wrapped == 0);
        CHECK(s.screen.row(2)[9].wrapped == 0);
        // Written over or erased: no longer.
        s.screen.write("\x1b[1;10Hx");
        CHECK(s.screen.row(0)[9].wrapped == 0);
        s.screen.write("\x1b[1;1H0123456789abc");
        CHECK(s.screen.row(0)[9].wrapped == 1);
        s.screen.write("\x1b[1;1H\x1b[2K");
        CHECK(s.screen.row(0)[9].wrapped == 0);
        // A wide character that doesn't fit wraps early; the blank left says so.
        Screen w(5);
        w.screen.write("abcd\xe3\x81\x82");
        CHECK(w.screen.row(0)[4].wrapped == 1);
        CHECK(w.screen.row(1)[0].wide == 1);
        // Scrolled into the history with the row.
        w.screen.write("\x1b[1;1H0123456789\r\n\n\n\n\n");
        uint16_t            length = 0;
        const TerminalCell *first = w.screen.historyRow(0, length); // "01234", wrapped to "56789"
        REQUIRE(w.screen.historyCount() == 3);
        REQUIRE(length == 5);
        CHECK(first[4].wrapped == 1);
        const TerminalCell *second = w.screen.historyRow(1, length);
        REQUIRE(length == 5);
        CHECK(second[4].wrapped == 0);
}

TEST_CASE("TerminalSelection: characters, from either end, a click selecting nothing") {
        Screen s(20);
        s.screen.write("hello world\r\nsecond line\r\nthird");
        s.begin(0, 6);
        CHECK(s.selection.isActive());
        CHECK(s.selection.isEmpty()); // a click
        CHECK(s.text().empty());
        s.extend(1, 6);
        CHECK(s.text() == "world\nsecond");
        // Dragged back past where it started.
        s.begin(1, 6);
        s.extend(0, 6);
        CHECK(s.text() == "world\nsecond");
        // To the next line's start: the line end only.
        s.begin(0, 6);
        s.extend(1, 0);
        CHECK(s.text() == "world\n");
        // Trailing blanks go; blanks chosen in the middle stay.
        s.begin(0, 0);
        s.extend(0, 20);
        CHECK(s.text() == "hello world");
        s.begin(0, 4);
        s.extend(0, 7);
        CHECK(s.text() == "o w");
        uint16_t from = 0, to = 0;
        CHECK(s.selection.columns(s.row(0), from, to));
        CHECK(from == 4);
        CHECK(to == 7);
        CHECK_FALSE(s.selection.columns(s.row(1), from, to));
        CHECK(s.selection.covers(s.row(0), s.row(3)));
        CHECK_FALSE(s.selection.covers(s.row(1), s.row(3)));
        s.selection.clear();
        CHECK_FALSE(s.selection.isActive());
        CHECK(s.text().empty());
}

TEST_CASE("TerminalSelection: words and lines") {
        Screen s(30);
        s.screen.write("ls -l /sd0/p0/a.txt  ===  x\r\n");
        s.begin(0, 9, Unit::Word); // in the path
        CHECK(s.text() == "/sd0/p0/a.txt");
        s.extend(0, 27); // dragged to "x"
        CHECK(s.text() == "/sd0/p0/a.txt  ===  x");
        s.begin(0, 22, Unit::Word); // punctuation: its run
        CHECK(s.text() == "===");
        s.begin(0, 20, Unit::Word); // blanks
        CHECK(s.text() == "  ");
        s.begin(0, 29, Unit::Word); // past the text: the blanks to the end, which go
        CHECK(s.text().empty());
        CHECK_FALSE(s.selection.isEmpty());

        // A line: its rows wrapped across, and its line end.
        Screen w(10);
        w.screen.write("0123456789abc\r\nnext\r\n");
        w.begin(1, 2, Unit::Line); // on the wrapped part
        CHECK(w.text() == "0123456789abc\n");
        w.extend(2, 0);
        CHECK(w.text() == "0123456789abc\nnext\n");
        // Characters across the wrap: joined.
        w.begin(0, 8, Unit::Character);
        w.extend(1, 2);
        CHECK(w.text() == "89ab");
}

TEST_CASE("TerminalSelection: blocks, wide characters, scrolling into the history") {
        Screen s(10);
        s.screen.write("abcdef\r\nghijkl\r\nmnopqr");
        s.begin(2, 3, Unit::Character, true);
        s.extend(0, 1);
        CHECK(s.text() == "bc\nhi\nno");
        uint16_t from = 0, to = 0;
        CHECK(s.selection.columns(s.row(1), from, to));
        CHECK(from == 1);
        CHECK(to == 3);

        Screen w(10);
        w.screen.write("a\xe3\x81\x82"
                       "b \xe2\x82\xac");
        w.begin(0, 0);
        w.extend(0, 10);
        CHECK(w.text() == "a\xe3\x81\x82"
                          "b \xe2\x82\xac");

        // Selected, then scrolled off the screen: the same text, now from the history.
        Screen h(10, 3);
        h.screen.write("keep me\r\n");
        h.begin(0, 0);
        h.extend(0, 4);
        CHECK(h.text() == "keep");
        h.screen.write("1\r\n2\r\n3\r\n4\r\n");
        CHECK(h.screen.historyCount() > 0);
        CHECK(h.text() == "keep");
        // Gone from the history too: nothing.
        Screen g(10, 2);
        REQUIRE(g.screen.setHistory(1).isOk());
        g.screen.write("old\r\n");
        g.begin(0, 0);
        g.extend(0, 3);
        g.screen.write("1\r\n2\r\n3\r\n4\r\n");
        CHECK(g.text().empty());
}

TEST_CASE("TerminalView: selected cells are drawn reversed") {
        Screen s(6, 1);
        s.screen.write("ab\x1b[7m"
                       "cd\x1b[0m"
                       "ef");
        const bmds::TerminalColors colors;
        bmds::TextCell             out[6];
        REQUIRE(TerminalView::cells(s.screen.row(0), 6, 6, colors, -1, out, 6, 1, 3) == 6);
        CHECK(out[0].fg == colors.text);
        CHECK(out[1].fg == colors.background); // selected
        CHECK(out[2].fg == colors.text);       // reversed and selected: plain
        CHECK(out[3].fg == colors.background); // reversed
        CHECK(out[4].fg == colors.text);
}

namespace {

        // What a screen's OSC 52 sink was told: "B:targets", "D:bytes", "E", "C", "Q:targets".
        struct Clip {
                        std::vector<std::string> events;

                        static void sink(void *context, TerminalScreen::ClipboardEvent event, StringView targets,
                                         bmds::ByteSpan data) {
                                auto             *self = static_cast<Clip *>(context);
                                const std::string t(targets.data(), targets.size());
                                using E = TerminalScreen::ClipboardEvent;
                                switch (event) {
                                        case E::Begin: self->events.push_back("B:" + t); break;
                                        case E::Data:
                                                // Pieces joined: what was set, not how it was cut.
                                                if (!self->events.empty() && self->events.back().rfind("D:", 0) == 0)
                                                        self->events.back().append(
                                                                reinterpret_cast<const char *>(data.data()),
                                                                data.size());
                                                else
                                                        self->events.push_back(
                                                                "D:" +
                                                                std::string(reinterpret_cast<const char *>(data.data()),
                                                                            data.size()));
                                                break;
                                        case E::End: self->events.push_back("E"); break;
                                        case E::Cancel: self->events.push_back("C"); break;
                                        case E::Query: self->events.push_back("Q:" + t); break;
                                }
                        }
        };

        // Writes @p text to a fresh screen with a sink, @p piece bytes at a time.
        std::string osc52(const std::string &text, size_t piece = 1000, std::string *title = nullptr) {
                TerminalScreen screen(bmds::MemSpace::Psram);
                REQUIRE(screen.resize(20, 4).isOk());
                Clip clip;
                screen.setClipboardSink(Clip::sink, &clip);
                for (size_t i = 0; i < text.size(); i += piece) {
                        const std::string part = text.substr(i, piece);
                        screen.write(StringView(part.data(), part.size()));
                }
                if (title != nullptr) *title = std::string(screen.title().data(), screen.title().size());
                std::string joined;
                for (const std::string &e : clip.events) joined += (joined.empty() ? "" : "|") + e;
                return joined;
        }

} // namespace

TEST_CASE("TerminalScreen: OSC 52 sets and asks for the clipboard, streamed") {
        // "hello world" in base64, ended by BEL or by ST (ESC \), split anywhere.
        const std::string set = "\x1b]52;c;aGVsbG8gd29ybGQ=\x07";
        for (size_t piece = 1; piece <= set.size(); ++piece) CHECK(osc52(set, piece) == "B:c|D:hello world|E");
        CHECK(osc52("\x1b]52;pc;aGk=\x1b\\after") == "B:pc|D:hi|E");
        CHECK(osc52("\x1b]52;;aGk\x07") == "B:|D:hi|E"); // no targets; unpadded
        CHECK(osc52("\x1b]52;c;\x07") == "B:c|E");       // emptied
        CHECK(osc52("\x1b]52;c;?\x07") == "Q:c");
        CHECK(osc52("\x1b]52;p;?\x1b\\") == "Q:p");
        // Not base64, or cut off: dropped.
        CHECK(osc52("\x1b]52;c;a$b\x07") == "B:c|C");
        CHECK(osc52("\x1b]52;c;aGVs\x18").empty()); // cut off before its first piece: nothing
        CHECK(osc52("\x1b]52;c;" + std::string(80, 'Q') + "\x18").rfind("B:c|D:", 0) == 0);
        CHECK(osc52("\x1b]52;c;" + std::string(80, 'Q') + "\x18").back() == 'C');
        CHECK(osc52("\x1b]52;c\x07").empty()); // no data part
        // Long: far past the parser's 64 bytes.
        std::string data;
        for (int i = 0; i < 3000; ++i) data.push_back(char('A' + i % 26));
        std::string encoded;
        {
                bmds::Base64Encoder e;
                std::vector<char>   buf(bmds::Base64Encoder::maxOutput(data.size()) + 4);
                size_t              n =
                        e.put(bmds::ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()), buf.data());
                n += e.finish(buf.data() + n);
                encoded.assign(buf.data(), n);
        }
        CHECK(osc52("\x1b]52;c;" + encoded + "\x07", 77) == "B:c|D:" + data + "|E");
        // Titles still work; other OSC numbers aren't streamed.
        std::string title;
        CHECK(osc52("\x1b]2;a title\x07\x1b]8;;http://x\x07", 1000, &title).empty());
        CHECK(title == "a title");
        // Without a sink, OSC 52 is ignored like any unknown OSC.
        TerminalScreen plain(bmds::MemSpace::Psram);
        REQUIRE(plain.resize(10, 2).isOk());
        plain.write("\x1b]52;c;aGk=\x07ok");
        CHECK(plain.row(0)[0].codepoint == 'o');
}
