/**
 * @file      lineeditor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <memory>
#include <string>
#include <bmds/lineeditor.h>
#include "fakeconsole.h"

using bmds::LineEditor;
using Event = LineEditor::Event;

namespace {

        constexpr const char *Up = "\x1b[A";
        constexpr const char *Down = "\x1b[B";
        constexpr const char *Right = "\x1b[C";
        constexpr const char *Left = "\x1b[D";

        // Feeds every byte; returns the last non-None event (or None).
        Event feed(LineEditor &ed, const std::string &bytes) {
                Event last = Event::None;
                for (char c : bytes) {
                        const Event ev = ed.feed(c);
                        if (ev != Event::None) last = ev;
                }
                return last;
        }

        std::string str(bmds::StringView v) {
                return std::string(v.data(), v.size());
        }

        std::string text(const LineEditor &ed) {
                return str(ed.line());
        }

} // namespace

TEST_CASE("LineEditor") {
        FakeConsole con;
        auto        ed = std::make_unique<LineEditor>(con);
        ed->start("> ");
        CHECK(con.take() == "> ");

        SUBCASE("typing echoes and Enter completes the line") {
                CHECK(feed(*ed, "mem") == Event::None);
                CHECK(con.take() == "mem");
                CHECK(feed(*ed, "\r") == Event::Line);
                CHECK(con.take() == "\n");
                CHECK(text(*ed) == "mem");
                CHECK(ed->lineData()[3] == '\0');
        }

        SUBCASE("CR LF is one Enter, LF alone also works") {
                CHECK(feed(*ed, "a\r") == Event::Line);
                ed->start("> ");
                CHECK(ed->feed('\n') == Event::None); // the LF of the CR LF pair
                CHECK(feed(*ed, "b\n") == Event::Line);
                CHECK(text(*ed) == "b");
        }

        SUBCASE("backspace") {
                feed(*ed, "abc");
                con.take();
                feed(*ed, "\x7f");
                CHECK(text(*ed) == "ab");
                CHECK(con.take() == "\x1b[1D\x1b[K");
                feed(*ed, "\b\b\b"); // BS too, and extra presses do nothing
                CHECK(text(*ed).empty());
        }

        SUBCASE("cursor movement and insertion in the middle") {
                feed(*ed, "ac");
                feed(*ed, Left);
                CHECK(ed->cursor() == 1);
                con.take();
                feed(*ed, "b");
                CHECK(text(*ed) == "abc");
                CHECK(ed->cursor() == 2);
                // Rewrites from the insertion point and moves back over the tail.
                CHECK(con.take() == "bc\x1b[K\x1b[1D");
                feed(*ed, Right);
                feed(*ed, Right); // at end: no movement
                CHECK(ed->cursor() == 3);
        }

        SUBCASE("home, end and delete") {
                feed(*ed, "hello");
                feed(*ed, "\x1b[H");
                CHECK(ed->cursor() == 0);
                feed(*ed, "\x1b[3~");
                CHECK(text(*ed) == "ello");
                feed(*ed, "\x1b[F");
                CHECK(ed->cursor() == 4);
                feed(*ed, "\x01"); // Ctrl-A
                CHECK(ed->cursor() == 0);
                feed(*ed, "\x05"); // Ctrl-E
                CHECK(ed->cursor() == 4);
                feed(*ed, "\x1bOH"); // SS3 Home
                CHECK(ed->cursor() == 0);
                feed(*ed, "\x1b[4~");
                CHECK(ed->cursor() == 4);
        }

        SUBCASE("word movement and kills") {
                feed(*ed, "log on Boot");
                feed(*ed, "\x1b[1;5D"); // Ctrl-Left
                CHECK(ed->cursor() == 7);
                feed(*ed, "\x1b"
                          "b"); // Alt-B
                CHECK(ed->cursor() == 4);
                feed(*ed, "\x1b[1;5C"); // Ctrl-Right
                CHECK(ed->cursor() == 6);
                feed(*ed, "\x17"); // Ctrl-W: kill "on"
                CHECK(text(*ed) == "log  Boot");
                CHECK(ed->cursor() == 4);
                feed(*ed, "\x0b"); // Ctrl-K
                CHECK(text(*ed) == "log ");
                feed(*ed, "\x15"); // Ctrl-U
                CHECK(text(*ed).empty());
                CHECK(ed->cursor() == 0);
        }

        SUBCASE("Ctrl-C cancels") {
                feed(*ed, "oops");
                con.take();
                CHECK(feed(*ed, "\x03") == Event::Cancel);
                CHECK(con.take() == "^C\n");
                CHECK(text(*ed).empty());
        }

        SUBCASE("Ctrl-D is EOF only on an empty line") {
                CHECK(feed(*ed, "\x04") == Event::Eof);
                feed(*ed, "ab");
                feed(*ed, Left);
                CHECK(feed(*ed, "\x04") == Event::None);
                CHECK(text(*ed) == "a");
        }

        SUBCASE("line length limit rings the bell") {
                feed(*ed, std::string(LineEditor::LineMax, 'x'));
                con.take();
                feed(*ed, "y");
                CHECK(con.take() == "\a");
                CHECK(ed->line().size() == LineEditor::LineMax);
        }

        SUBCASE("unknown sequences and control bytes are ignored") {
                feed(*ed, "a\x1b[99~\x1b[5;9Z\t\x1bZ"
                          "b");
                CHECK(text(*ed) == "ab");
        }

        SUBCASE("history") {
                feed(*ed, "one\r");
                ed->start("> ");
                feed(*ed, "two\r");
                ed->start("> ");
                feed(*ed, "two\r"); // repeat of the newest: not added again
                ed->start("> ");
                feed(*ed, "\r"); // empty: not added
                CHECK(ed->historyCount() == 2);
                CHECK(str(ed->historyEntry(0)) == "two");
                CHECK(str(ed->historyEntry(1)) == "one");
                CHECK(ed->historyEntry(2).isEmpty());

                ed->start("> ");
                feed(*ed, "dra");
                feed(*ed, Up);
                CHECK(text(*ed) == "two");
                feed(*ed, Up);
                CHECK(text(*ed) == "one");
                feed(*ed, Up); // oldest: stays
                CHECK(text(*ed) == "one");
                feed(*ed, Down);
                CHECK(text(*ed) == "two");
                feed(*ed, Down); // back to the line being typed
                CHECK(text(*ed) == "dra");
                CHECK(ed->cursor() == 3);
                feed(*ed, "\x10"); // Ctrl-P
                CHECK(text(*ed) == "two");
                feed(*ed, "!\r");
                CHECK(text(*ed) == "two!");
                CHECK(str(ed->historyEntry(0)) == "two!");
        }

        SUBCASE("history evicts the oldest entries") {
                const std::string big(250, 'a');
                for (char c : std::string("12345")) {
                        ed->start("> ");
                        feed(*ed, big + c + "\r");
                }
                // Entries take 252 bytes each; four fit in 1024, so "1" was evicted.
                CHECK(ed->historyCount() == 4);
                CHECK(str(ed->historyEntry(0)) == big + "5");
                CHECK(str(ed->historyEntry(3)) == big + "2");
        }

        SUBCASE("redraw and clear screen") {
                feed(*ed, "abc");
                feed(*ed, Left);
                con.take();
                ed->redraw();
                CHECK(con.take() == "\r> abc\x1b[K\x1b[1D");
                feed(*ed, "\x0c"); // Ctrl-L
                CHECK(con.take() == "\x1b[H\x1b[2J> abc\x1b[1D");
        }
}
