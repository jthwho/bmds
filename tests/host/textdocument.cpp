/**
 * @file      textdocument.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TextDocument and TextCursor (devplan/editor.md §3).
 */

#include <doctest.h>

#include <string>
#include <bmds/textdocument.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::MemSpace;
using bmds::String;
using bmds::StringView;
using bmds::TextCursor;
using bmds::TextDocument;
using bmds::TextPosition;
using bmds::TextRange;
using bmds::TextSelection;

namespace {

        std::string saved(const TextDocument &d) {
                String out(MemSpace::Psram);
                REQUIRE(d.save(out).isOk());
                CHECK(out.size() == d.savedSize());
                return std::string(out.data(), out.size());
        }

        std::string all(const TextDocument &d) {
                String out(MemSpace::Psram);
                REQUIRE(d.text(TextRange{{0, 0}, d.end()}, out).isOk());
                return std::string(out.data(), out.size());
        }

        TextPosition at(uint32_t line, uint32_t byte) {
                return TextPosition{line, byte};
        }

        struct Recorder {
                        TextDocument::Change last{};
                        int                  count = 0;
                        static void          changed(void *arg, const TextDocument::Change &c) {
                                auto *r = static_cast<Recorder *>(arg);
                                r->last = c;
                                ++r->count;
                        }
        };

        // A typed character, as an editor types it: one edit in a mergeable step.
        void type(TextDocument &d, TextCursor &c, const char *ch, uint32_t ms) {
                const TextSelection before = c.selection();
                d.beginEdit(before, TextDocument::EditKind::Typing, ms, ch);
                auto r = d.replace(c.range(), ch);
                REQUIRE(r.isOk());
                c.setPosition(r.value());
                d.endEdit(c.selection());
        }

} // namespace

TEST_CASE("TextDocument: loading and saving give back the same bytes") {
        const char *cases[] = {"",
                               "one line",
                               "a\nb\n",
                               "a\nb",
                               "\n\n\n",
                               "dos\r\nlines\r\n",
                               "mixed\r\nends\nhere\r\n",
                               "\xEF\xBB\xBF"
                               "with a mark\n",
                               "bad \xFF\xFE utf-8 \xC3 and NUL \0 kept"};
        for (const char *c : cases) {
                const std::string bytes = std::string(c) == "bad \xFF\xFE utf-8 \xC3 and NUL "
                                                  ? std::string("bad \xFF\xFE utf-8 \xC3 and NUL \0 kept", 33)
                                                  : std::string(c);
                TextDocument      d;
                REQUIRE(d.load(StringView(bytes.data(), bytes.size())).isOk());
                CHECK(saved(d) == bytes);
                CHECK_FALSE(d.isModified());
        }
        TextDocument d;
        REQUIRE(d.load("a\nb\n").isOk());
        CHECK(d.lineCount() == 3);
        CHECK(d.line(2).isEmpty());
        CHECK(d.length() == 4);
        CHECK(d.lineEnding() == TextDocument::LineEnding::Lf);
        REQUIRE(d.load("x\r\ny").isOk());
        CHECK(d.lineEnding() == TextDocument::LineEnding::CrLf);
        CHECK(d.line(0) == "x");
        REQUIRE(d.load("\xEF\xBB\xBFz").isOk());
        CHECK(d.hasByteOrderMark());
        CHECK(d.line(0) == "z");
}

TEST_CASE("TextDocument: inserting and removing, within lines and across them") {
        TextDocument d;
        Recorder     rec;
        REQUIRE(d.addObserver(Recorder::changed, &rec).isOk());
        REQUIRE(d.load("hello world").isOk());
        auto r = d.insert(at(0, 5), ",");
        REQUIRE(r.isOk());
        CHECK(r.value() == at(0, 6));
        CHECK(all(d) == "hello, world");
        CHECK(rec.last.firstLine == 0);
        CHECK(rec.last.removedLines == 1);
        CHECK(rec.last.insertedLines == 1);

        r = d.insert(at(0, 6), "\nbig\n");
        REQUIRE(r.isOk());
        CHECK(r.value() == at(2, 0));
        CHECK(all(d) == "hello,\nbig\n world");
        CHECK(d.lineCount() == 3);
        CHECK(rec.last.insertedLines == 3);
        CHECK(d.length() == 17);

        REQUIRE(d.remove(TextRange{at(0, 3), at(2, 2)}).isOk());
        CHECK(all(d) == "helorld");
        CHECK(d.lineCount() == 1);
        CHECK(rec.last.removedLines == 3);
        CHECK(rec.last.insertedLines == 1);
        CHECK(d.length() == 7);

        // CR LF in inserted text is a newline; a lone CR stays.
        r = d.insert(at(0, 3), "a\r\nb\rc");
        REQUIRE(r.isOk());
        CHECK(all(d) == "hela\nb\rcorld");
        r = d.replace(TextRange{at(0, 0), d.end()}, "new");
        REQUIRE(r.isOk());
        CHECK(all(d) == "new");
        // Positions past the end are clamped.
        r = d.insert(at(9, 9), "!");
        CHECK(all(d) == "new!");
        d.removeObserver(Recorder::changed, &rec);
}

TEST_CASE("TextDocument: new lines take the file's ending, a split line keeps its own") {
        TextDocument d;
        REQUIRE(d.load("a\r\nb\r\n").isOk());
        REQUIRE(d.insert(at(0, 1), "\nx").isOk());
        CHECK(saved(d) == "a\r\nx\r\nb\r\n");
        REQUIRE(d.load("one\ntwo\r\n").isOk()); // the first line's: LF
        REQUIRE(d.insert(at(1, 3), "\nthree").isOk());
        CHECK(saved(d) == "one\ntwo\nthree\r\n");
        d.setLineEnding(TextDocument::LineEnding::CrLf, true);
        CHECK(saved(d) == "one\r\ntwo\r\nthree\r\n");
        CHECK(d.isModified());
}

TEST_CASE("TextDocument: undo and redo put text and selection back") {
        TextDocument d;
        REQUIRE(d.load("abc").isOk());
        TextSelection before{at(0, 1), at(0, 1)};
        d.beginEdit(before);
        REQUIRE(d.insert(at(0, 1), "XY").isOk());
        REQUIRE(d.remove(TextRange{at(0, 4), at(0, 5)}).isOk());
        d.endEdit(TextSelection{at(0, 3), at(0, 3)});
        CHECK(all(d) == "aXYb");
        CHECK(d.isModified());
        auto u = d.undo();
        REQUIRE(u.isOk());
        CHECK(u.value() == before);
        CHECK(all(d) == "abc");
        CHECK_FALSE(d.isModified());
        auto r = d.redo();
        REQUIRE(r.isOk());
        CHECK(r.value().cursor == at(0, 3));
        CHECK(all(d) == "aXYb");
        CHECK(d.undo().isOk());
        CHECK(d.undo().error() == Error::Empty);
        CHECK(d.canRedo());
        // A new edit drops what could be redone.
        REQUIRE(d.insert(at(0, 0), "!").isOk());
        CHECK_FALSE(d.canRedo());
        // An edit outside beginEdit is a step of its own.
        CHECK(d.undo().isOk());
        CHECK(all(d) == "abc");

        // Across lines.
        REQUIRE(d.load("1\n2\n3").isOk());
        REQUIRE(d.replace(TextRange{at(0, 1), at(2, 0)}, "-\n-").isOk());
        CHECK(all(d) == "1-\n-3");
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "1\n2\n3");
        REQUIRE(d.redo().isOk());
        CHECK(all(d) == "1-\n-3");
}

TEST_CASE("TextDocument: typing merges by words, pauses and moves") {
        TextDocument d;
        TextCursor   c(d);
        const char  *word1[] = {"h", "i"};
        uint32_t     ms = 1000;
        for (const char *ch : word1) type(d, c, ch, ms += 50);
        type(d, c, " ", ms += 50); // a space joins the word before it
        type(d, c, "y", ms += 50); // a word after a space starts a step
        type(d, c, "o", ms += 50);
        CHECK(all(d) == "hi yo");
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "hi ");
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "");
        CHECK_FALSE(d.canUndo());
        REQUIRE(d.redo().isOk());
        REQUIRE(d.redo().isOk());
        CHECK(all(d) == "hi yo");

        // A pause starts a step.
        c.setPosition(d.end());
        type(d, c, "u", ms += 50);
        type(d, c, "r", ms += TextDocument::MergeMs + 1);
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "hi you");
        // A move starts one.
        c.setPosition(d.end());
        type(d, c, "!", ms += 10);
        d.breakMerge();
        type(d, c, "!", ms += 10);
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "hi you!");

        // Typing doesn't merge into the step the file was saved at.
        TextDocument s;
        TextCursor   sc(s);
        type(s, sc, "a", 100);
        s.setSaved();
        type(s, sc, "b", 150);
        CHECK(s.isModified());
        REQUIRE(s.undo().isOk());
        CHECK_FALSE(s.isModified());
        CHECK(all(s) == "a");
}

TEST_CASE("TextDocument: the saved mark through undo, redo and a lost step") {
        TextDocument d;
        REQUIRE(d.load("x").isOk());
        REQUIRE(d.insert(at(0, 1), "1").isOk());
        REQUIRE(d.insert(at(0, 2), "2").isOk());
        d.setSaved();
        CHECK_FALSE(d.isModified());
        REQUIRE(d.undo().isOk());
        CHECK(d.isModified());
        REQUIRE(d.redo().isOk());
        CHECK_FALSE(d.isModified());
        // Undo past the save, then a new edit: the saved text can't come back.
        REQUIRE(d.undo().isOk());
        REQUIRE(d.insert(at(0, 0), "z").isOk());
        CHECK(d.isModified());
        REQUIRE(d.undo().isOk());
        CHECK(d.isModified());
        // Clearing the log keeps the state.
        d.clearUndo();
        CHECK(d.isModified());
}

TEST_CASE("TextDocument: the undo log's limit drops the oldest steps") {
        TextDocument d;
        d.setUndoLimit(100);
        for (int i = 0; i < 20; ++i) REQUIRE(d.insert(d.end(), "0123456789").isOk());
        CHECK(d.undoBytes() <= 100);
        int undone = 0;
        while (d.undo().isOk()) ++undone;
        CHECK(undone == 10);
        CHECK(d.length() == 100);
        CHECK(d.isModified()); // the loaded text is gone from the log
}

TEST_CASE("TextDocument: find, forward and back, with the flags") {
        TextDocument d;
        REQUIRE(d.load("Cat cat\ncatalog\nthe CAT").isOk());
        using TD = TextDocument;
        auto f = d.find("cat", at(0, 0), 0);
        REQUIRE(f.isOk());
        CHECK(f.value() == TextRange{at(0, 0), at(0, 3)});
        f = d.find("cat", at(0, 1), TD::FindCaseSensitive);
        CHECK(f.value() == TextRange{at(0, 4), at(0, 7)});
        f = d.find("cat", at(0, 5), TD::FindWholeWord);
        CHECK(f.value().start == at(2, 4)); // "catalog" isn't the whole word
        f = d.find("cat", at(2, 5), 0);
        CHECK(f.error() == Error::NotFound);
        f = d.find("cat", at(2, 5), TD::FindWrap);
        CHECK(f.value().start == at(0, 0));
        f = d.find("cat", at(1, 0), TD::FindBackward);
        CHECK(f.value().start == at(0, 4));
        f = d.find("cat", at(0, 0), TD::FindBackward);
        CHECK(f.error() == Error::NotFound);
        f = d.find("cat", at(0, 0), TD::FindBackward | TD::FindWrap);
        CHECK(f.value().start == at(2, 4));
        CHECK(d.find("", at(0, 0), 0).error() == Error::Invalid);
        CHECK(d.find("a\nb", at(0, 0), 0).error() == Error::Invalid);

        auto n = d.replaceAll("cat", "dog", TD::FindWholeWord);
        REQUIRE(n.isOk());
        CHECK(n.value() == 3);
        CHECK(all(d) == "dog dog\ncatalog\nthe dog");
        REQUIRE(d.undo().isOk()); // one step
        CHECK(all(d) == "Cat cat\ncatalog\nthe CAT");
        // A replacement holding the needle doesn't repeat forever.
        n = d.replaceAll("a", "aa", TD::FindCaseSensitive);
        CHECK(n.value() == 4);
}

TEST_CASE("TextDocument: characters, words and columns") {
        TextDocument d;
        REQUIRE(d.load("h\xC3\xA9llo  w_1, x\n\t\xE4\xB8\xAD!").isOk()); // é, then a tab and a wide 中
        CHECK(d.nextCharacter(at(0, 1)) == at(0, 3));
        CHECK(d.previousCharacter(at(0, 3)) == at(0, 1));
        CHECK(d.nextCharacter(d.clamp(at(0, 99))) == at(1, 0));
        CHECK(d.previousCharacter(at(1, 0)) == at(0, 14));
        CHECK(d.nextWord(at(0, 0)) == at(0, 8));   // past "héllo" and its spaces
        CHECK(d.nextWord(at(0, 8)) == at(0, 11));  // "w_1"
        CHECK(d.nextWord(at(0, 11)) == at(0, 13)); // the comma and its space
        CHECK(d.previousWord(at(0, 8)) == at(0, 0));
        CHECK(d.previousWord(at(0, 11)) == at(0, 8));
        CHECK(d.wordAt(at(0, 2)) == TextRange{at(0, 0), at(0, 6)});
        CHECK(d.wordAt(at(0, 11)) == TextRange{at(0, 8), at(0, 11)}); // just after a word
        CHECK(d.wordAt(at(0, 6)).isEmpty() == false);
        CHECK(d.columnOf(0, 6, 8) == 5);  // é is one column
        CHECK(d.columnOf(1, 1, 8) == 8);  // the tab
        CHECK(d.columnOf(1, 4, 8) == 10); // 中 is two
        CHECK(d.columnOf(1, 1, 4) == 4);
        CHECK(d.byteAtColumn(1, 3, 8) == 0); // nearer the tab's start
        CHECK(d.byteAtColumn(1, 5, 8) == 1); // nearer its end
        CHECK(d.byteAtColumn(1, 9, 8) == 4); // the second half of 中
        CHECK(d.byteAtColumn(1, 99, 8) == 5);
        // A stray continuation byte is a character of its own.
        REQUIRE(d.load("a\x80"
                       "b")
                        .isOk());
        CHECK(d.nextCharacter(at(0, 1)) == at(0, 2));
        CHECK(d.previousCharacter(at(0, 2)) == at(0, 1));
}

TEST_CASE("TextCursor: moves and selections") {
        TextDocument d;
        REQUIRE(d.load("    indented line\nx\nlonger line here").isOk());
        TextCursor c(d);
        c.setPosition(at(0, 12));
        c.moveDown(false);
        CHECK(c.position() == at(1, 1)); // the short line's end
        c.moveDown(false);
        CHECK(c.position() == at(2, 12)); // the column wanted, kept
        c.moveUp(true, 2);
        CHECK(c.position() == at(0, 12));
        CHECK(c.anchor() == at(2, 12));
        c.moveLeft(false); // a selection collapses to its start
        CHECK(c.position() == at(0, 12));
        CHECK_FALSE(c.hasSelection());
        c.moveLineStart(false);
        CHECK(c.position() == at(0, 4)); // the first non-blank
        c.moveLineStart(false);
        CHECK(c.position() == at(0, 0));
        c.moveLineStart(false);
        CHECK(c.position() == at(0, 4));
        c.moveLineEnd(true);
        CHECK(c.range() == TextRange{at(0, 4), at(0, 17)});
        c.moveUp(false);
        CHECK(c.position() == at(0, 0)); // the first line: to its start
        c.moveDocumentEnd(false);
        CHECK(c.position() == at(2, 16));
        c.moveDown(false);
        CHECK(c.position() == at(2, 16));
        c.setPosition(at(2, 3));
        c.selectWord();
        CHECK(c.range() == TextRange{at(2, 0), at(2, 6)});
        c.selectLine();
        CHECK(c.range() == TextRange{at(2, 0), at(2, 16)});
        c.setPosition(at(1, 0));
        c.selectLine();
        CHECK(c.range() == TextRange{at(1, 0), at(2, 0)});
        c.selectAll();
        CHECK(c.range() == TextRange{at(0, 0), at(2, 16)});
        c.moveWordRight(false);
        CHECK(c.position() == at(2, 16));
        c.setPosition(at(0, 99));
        CHECK(c.position() == at(0, 17));
}

TEST_CASE("TextDocument: running out of memory changes nothing") {
        TextDocument d;
        REQUIRE(d.load("start\nend").isOk());
        const std::string before = all(d);
        for (long n = 0; n < 12; ++n) {
                bool ok;
                {
                        HostMemory::FailGuard fail(n);
                        ok = d.insert(at(0, 2), "a\nlonger line\nb\n").isOk();
                }
                if (ok) {
                        CHECK(all(d) == "sta\nlonger line\nb\nart\nend");
                        break;
                }
                CHECK(all(d) == before);
        }
}

TEST_CASE("TextDocument: a large file") {
        std::string big;
        for (int i = 0; i < 100000; ++i) big += "line " + std::to_string(i) + "\n";
        TextDocument d;
        REQUIRE(d.load(StringView(big.data(), big.size())).isOk());
        CHECK(d.lineCount() == 100001);
        CHECK(d.line(99999) == "line 99999");
        REQUIRE(d.insert(at(50000, 0), "x\ny\n").isOk());
        CHECK(d.lineCount() == 100003);
        CHECK(d.line(50002) == "line 50000");
        auto f = d.find("line 99999", at(0, 0), bmds::TextDocument::FindCaseSensitive);
        REQUIRE(f.isOk());
        CHECK(f.value().start.line == 100001);
        CHECK(saved(d).size() == big.size() + 4);
}

TEST_CASE("TextDocument: lines in the loaded block, edited in place and grown") {
        TextDocument d;
        REQUIRE(d.load("alpha\nbravo\ncharlie\ndelta").isOk());
        REQUIRE(d.remove(TextRange{at(1, 1), at(1, 3)}).isOk()); // shrinks in place
        CHECK(all(d) == "alpha\nbvo\ncharlie\ndelta");
        REQUIRE(d.insert(at(1, 3), "-grown-past-its-place").isOk()); // its own block now
        CHECK(all(d) == "alpha\nbvo-grown-past-its-place\ncharlie\ndelta");
        REQUIRE(d.remove(TextRange{at(2, 2), at(3, 1)}).isOk()); // joined, shorter than the first was
        CHECK(all(d) == "alpha\nbvo-grown-past-its-place\nchelta");
        REQUIRE(d.insert(at(0, 2), "\n").isOk()); // split in place
        CHECK(all(d) == "al\npha\nbvo-grown-past-its-place\nchelta");
        REQUIRE(d.undo().isOk());
        REQUIRE(d.undo().isOk());
        REQUIRE(d.undo().isOk());
        REQUIRE(d.undo().isOk());
        CHECK(all(d) == "alpha\nbravo\ncharlie\ndelta");
        CHECK_FALSE(d.isModified());
        REQUIRE(d.load("x").isOk()); // the old block freed (the leak check sees it)
        CHECK(all(d) == "x");
}
