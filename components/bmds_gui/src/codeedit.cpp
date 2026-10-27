/**
 * @file      codeedit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A code editor on TextEdit (devplan/editor.md §6).
 */

#include <bmds/codeedit.h>

#include <cstdio>
#include <bmds/bitmapfont.h>
#include <bmds/glyphrenderer.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Kind = TextDocument::EditKind;

        constexpr uint16_t LineHeight = 16;     // unscii-16's: the editors draw in its cells
        constexpr uint16_t MarkWidth = 8;       // the diagnostic mark's column, left of the numbers
        constexpr uint16_t BreakMarkWidth = 16; // with breakpoints: their dot, the arrow, then the mark
        constexpr uint32_t MatchLines = 2000; // how far a bracket's match is looked for
        constexpr uint8_t  SlashKey = 0x38;   // HID's `/`

        constexpr size_t S(SyntaxStyle s) {
                return static_cast<size_t>(s);
        }

        constexpr uint8_t B(SyntaxStyle s) {
                return static_cast<uint8_t>(1u << S(s));
        }

        // Light, as the desktop's fields.
        constexpr CodeEdit::Scheme Classic = [] {
                CodeEdit::Scheme s;
                s.id = "classic";
                s.name = "Classic";
                s.style[S(SyntaxStyle::Normal)] = 0x000000;
                s.style[S(SyntaxStyle::Keyword)] = 0x0000c0;
                s.style[S(SyntaxStyle::Type)] = 0x006c80;
                s.style[S(SyntaxStyle::Constant)] = 0x8000a0;
                s.style[S(SyntaxStyle::Number)] = 0x008060;
                s.style[S(SyntaxStyle::String)] = 0xa01818;
                s.style[S(SyntaxStyle::Character)] = 0xa01818;
                s.style[S(SyntaxStyle::Comment)] = 0x007000;
                s.style[S(SyntaxStyle::Preprocessor)] = 0x800080;
                s.style[S(SyntaxStyle::Operator)] = 0x404040;
                s.style[S(SyntaxStyle::Function)] = 0x704000;
                s.style[S(SyntaxStyle::Heading)] = 0x0000c0;
                s.style[S(SyntaxStyle::Emphasis)] = 0x000000;
                s.style[S(SyntaxStyle::Added)] = 0x007000;
                s.style[S(SyntaxStyle::Removed)] = 0xc00000;
                s.style[S(SyntaxStyle::Error)] = 0xff0000;
                s.bold = B(SyntaxStyle::Heading) | B(SyntaxStyle::Emphasis);
                s.background = 0xffffff;
                s.currentLine = 0xeef2fc;
                s.selectionText = 0xffffff;
                s.selectionBackground = 0x000080;
                s.cursor = 0x000000;
                s.gutterBackground = 0xe4e4e4;
                s.gutterText = 0x909090;
                s.gutterCurrent = 0x000000;
                s.bracket = 0xc8dcff;
                s.error = 0xe00000;
                s.warning = 0xd09000;
                s.breakpoint = 0xc81e1e;
                s.execution = 0xfff3a0;
                s.executionMark = 0xe0a000;
                return s;
        }();

        // QBasic's: light grey on blue.
        constexpr CodeEdit::Scheme QBasic = [] {
                CodeEdit::Scheme s;
                s.id = "qbasic";
                s.name = "QBasic";
                s.style[S(SyntaxStyle::Normal)] = 0xaaaaaa;
                s.style[S(SyntaxStyle::Keyword)] = 0xffffff;
                s.style[S(SyntaxStyle::Type)] = 0x55ffff;
                s.style[S(SyntaxStyle::Constant)] = 0xff55ff;
                s.style[S(SyntaxStyle::Number)] = 0xffff55;
                s.style[S(SyntaxStyle::String)] = 0x55ff55;
                s.style[S(SyntaxStyle::Character)] = 0x55ff55;
                s.style[S(SyntaxStyle::Comment)] = 0x00aaaa;
                s.style[S(SyntaxStyle::Preprocessor)] = 0xff55ff;
                s.style[S(SyntaxStyle::Operator)] = 0xaaaaaa;
                s.style[S(SyntaxStyle::Function)] = 0xffffff;
                s.style[S(SyntaxStyle::Heading)] = 0xffffff;
                s.style[S(SyntaxStyle::Emphasis)] = 0xffffff;
                s.style[S(SyntaxStyle::Added)] = 0x55ff55;
                s.style[S(SyntaxStyle::Removed)] = 0xff5555;
                s.style[S(SyntaxStyle::Error)] = 0xff5555;
                s.bold = B(SyntaxStyle::Heading);
                s.background = 0x0000aa;
                s.currentLine = 0x0000aa;
                s.selectionText = 0x0000aa;
                s.selectionBackground = 0xaaaaaa;
                s.cursor = 0xffffff;
                s.gutterBackground = 0x000080;
                s.gutterText = 0x5555ff;
                s.gutterCurrent = 0xffffff;
                s.bracket = 0x0000ff;
                s.error = 0xff5555;
                s.warning = 0xffff55;
                s.breakpoint = 0xff5555;
                s.execution = 0xaa5500;
                s.executionMark = 0xffff55;
                return s;
        }();

        constexpr CodeEdit::Scheme Dark = [] {
                CodeEdit::Scheme s;
                s.id = "dark";
                s.name = "Dark";
                s.style[S(SyntaxStyle::Normal)] = 0xd4d4d4;
                s.style[S(SyntaxStyle::Keyword)] = 0x569cd6;
                s.style[S(SyntaxStyle::Type)] = 0x4ec9b0;
                s.style[S(SyntaxStyle::Constant)] = 0x569cd6;
                s.style[S(SyntaxStyle::Number)] = 0xb5cea8;
                s.style[S(SyntaxStyle::String)] = 0xce9178;
                s.style[S(SyntaxStyle::Character)] = 0xce9178;
                s.style[S(SyntaxStyle::Comment)] = 0x6a9955;
                s.style[S(SyntaxStyle::Preprocessor)] = 0xc586c0;
                s.style[S(SyntaxStyle::Operator)] = 0xd4d4d4;
                s.style[S(SyntaxStyle::Function)] = 0xdcdcaa;
                s.style[S(SyntaxStyle::Heading)] = 0x569cd6;
                s.style[S(SyntaxStyle::Emphasis)] = 0xd4d4d4;
                s.style[S(SyntaxStyle::Added)] = 0x6a9955;
                s.style[S(SyntaxStyle::Removed)] = 0xf44747;
                s.style[S(SyntaxStyle::Error)] = 0xf44747;
                s.bold = B(SyntaxStyle::Heading) | B(SyntaxStyle::Emphasis);
                s.background = 0x1e1e1e;
                s.currentLine = 0x282828;
                s.selectionText = 0xffffff;
                s.selectionBackground = 0x264f78;
                s.cursor = 0xd4d4d4;
                s.gutterBackground = 0x1e1e1e;
                s.gutterText = 0x6e6e6e;
                s.gutterCurrent = 0xc6c6c6;
                s.bracket = 0x3a4a5a;
                s.error = 0xf44747;
                s.warning = 0xcca700;
                s.breakpoint = 0xe51400;
                s.execution = 0x4b4b18;
                s.executionMark = 0xffcc00;
                return s;
        }();

        constexpr CodeEdit::Scheme Schemes[] = {Classic, QBasic, Dark};

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

        char closerOf(char c) {
                return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : 0;
        }

        char openerOf(char c) {
                return c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
        }

} // namespace

Span<const CodeEdit::Scheme> CodeEdit::schemes() {
        return Span<const Scheme>(Schemes);
}

const CodeEdit::Scheme *CodeEdit::scheme(StringView id) {
        for (const Scheme &s : Schemes)
                if (StringView(s.id) == id) return &s;
        return nullptr;
}

CodeEdit::CodeEdit(Widget *parent) : TextEdit(parent) {
        setScheme(Schemes[0]);
        setAutoIndent(true);
        _digits = digits();
}

CodeEdit::~CodeEdit() {
        GuiWindow *w = window();
        if (w != nullptr) w->application().forgetToolTip(this);
}

void CodeEdit::setSyntax(const Syntax *syntax) {
        _syntax = syntax;
        _validUpTo = 0;
        _resyncFrom = _resyncEnd = 0;
        matchBrackets();
        update();
        return;
}

void CodeEdit::setTypeNames(List<String> &&names) {
        _typeNames = std::move(names);
        update();
        return;
}

// Words in the gaps between the scanner's spans that are among the type names: spans of their
// own, in order.
size_t CodeEdit::addTypeNames(StringView text, StyleSpan *spans, size_t count, size_t max) const {
        if (_typeNames.isEmpty()) return count;
        StyleSpan  out[TextEdit::MaxRuns];
        size_t     n = 0, k = 0, i = 0;
        const auto isWord = [](char c) {
                return TextDocument::isWordByte(c);
        };
        while (i < text.size() && n < max && n < MaxRuns) {
                if (k < count && spans[k].start <= i) {
                        out[n++] = spans[k];
                        i = spans[k].end;
                        ++k;
                        continue;
                }
                const size_t limit = k < count ? spans[k].start : text.size();
                if (!isWord(text[i]) || (i > 0 && isWord(text[i - 1]))) {
                        ++i;
                        continue;
                }
                size_t j = i;
                while (j < limit && isWord(text[j])) ++j;
                const StringView word = text.mid(i, j - i);
                size_t           lo = 0, hi = _typeNames.size();
                bool             found = false;
                while (lo < hi && !found) {
                        const size_t mid = (lo + hi) / 2;
                        const auto   c = word <=> _typeNames[mid].view();
                        if (c == 0)
                                found = true;
                        else if (c < 0)
                                hi = mid;
                        else
                                lo = mid + 1;
                }
                if (found && !(word[0] >= '0' && word[0] <= '9'))
                        out[n++] = StyleSpan{uint32_t(i), uint32_t(j), SyntaxStyle::Type};
                i = j;
        }
        while (k < count && n < max && n < MaxRuns) out[n++] = spans[k++];
        for (size_t m = 0; m < n; ++m) spans[m] = out[m];
        return n;
}

void CodeEdit::setScheme(const Scheme &scheme) {
        _scheme = scheme;
        Colors c;
        c.text = scheme.style[S(SyntaxStyle::Normal)];
        c.background = scheme.background;
        c.selectionText = scheme.selectionText;
        c.selectionBackground = scheme.selectionBackground;
        c.cursor = scheme.cursor;
        c.currentLine = scheme.currentLine;
        setColors(c);
        return;
}

void CodeEdit::setLineNumbers(bool on) {
        if (on == _lineNumbers) return;
        _lineNumbers = on;
        relayoutContent();
        return;
}

// ---- Scanner states

void CodeEdit::documentChanged(const TextDocument::Change &change) {
        // The states after the change still hold from where they were known, if the line before
        // them ends as it did: scanning stops early once a line's end state is as it was.
        const int64_t delta = int64_t(change.insertedLines) - int64_t(change.removedLines);
        if (change.firstLine < _validUpTo) {
                const int64_t end = int64_t(_validUpTo) + delta;
                _resyncFrom = change.firstLine + change.insertedLines;
                _resyncEnd = end > int64_t(_resyncFrom) ? uint32_t(end) : _resyncFrom;
                _validUpTo = change.firstLine;
        } else if (_resyncEnd > change.firstLine) {
                _resyncEnd = change.firstLine; // nothing past this change can be trusted
        }
        // Diagnostics after the change move with their lines; those on changed lines stay where they were.
        const auto moved = [&](uint32_t line) {
                if (line >= change.firstLine + change.removedLines) return uint32_t(int64_t(line) + delta);
                if (line >= change.firstLine + change.insertedLines) return change.firstLine;
                return line;
        };
        if (delta != 0 && !_diagnostics.isEmpty()) {
                for (Diagnostic &d : _diagnostics) d.line = moved(d.line);
        }
        // So do breakpoints (two on lines joined become one) and the line about to run.
        if (delta != 0 && !_breakpoints.isEmpty()) {
                bool   changed = false;
                size_t kept = 0;
                for (size_t i = 0; i < _breakpoints.size(); ++i) {
                        const uint32_t line = moved(_breakpoints[i]);
                        changed = changed || line != _breakpoints[i];
                        if (kept > 0 && _breakpoints[kept - 1] == line) continue;
                        _breakpoints[kept++] = line;
                }
                changed = changed || kept != _breakpoints.size();
                while (_breakpoints.size() > kept) _breakpoints.removeAt(_breakpoints.size() - 1);
                if (changed) tellBreakpoints();
        }
        if (delta != 0 && _executionLine != NoLine) _executionLine = moved(_executionLine);
        const uint32_t now = digits();
        if (now != _digits) {
                _digits = now;
                relayoutContent();
        }
        return;
}

void CodeEdit::ensureStates(uint32_t line) {
        if (_syntax == nullptr) return;
        TextDocument &doc = document();
        StyleSpan     spans[1];
        while (_validUpTo < line && _validUpTo < doc.lineCount()) {
                const uint32_t j = _validUpTo;
                const uint16_t start = j == 0 ? SyntaxState::Normal : doc.lineState(j - 1);
                size_t         count = 0;
                const uint16_t end = _syntax->scan(doc.line(j), start, spans, 1, count);
                const uint16_t old = doc.lineState(j);
                doc.setLineState(j, end);
                ++_validUpTo;
                if (j >= _resyncFrom && j < _resyncEnd && end == old) {
                        _validUpTo = _resyncEnd;
                        _resyncFrom = _resyncEnd = 0;
                }
        }
        return;
}

uint16_t CodeEdit::startState(uint32_t line) {
        if (line == 0 || _syntax == nullptr) return SyntaxState::Normal;
        ensureStates(line);
        return document().lineState(line - 1);
}

size_t CodeEdit::styleLine(uint32_t line, StringView text, StyleRun *runs, size_t max) {
        size_t n = 0;
        if (_syntax != nullptr) {
                StyleSpan      spans[TextEdit::MaxRuns];
                size_t         count = 0;
                const uint16_t start = startState(line);
                const uint16_t end = _syntax->scan(text, start, spans, max < MaxRuns ? max : MaxRuns, count);
                count = addTypeNames(text, spans, count, max < MaxRuns ? max : MaxRuns);
                // Drawn lines are scanned in order: this one's end state, kept, extends what is known.
                if (line == _validUpTo) {
                        document().setLineState(line, end);
                        ++_validUpTo;
                }
                for (size_t i = 0; i < count && n < max; ++i) {
                        const size_t st = S(spans[i].style);
                        runs[n].start = spans[i].start;
                        runs[n].end = spans[i].end;
                        runs[n].text = _scheme.style[st];
                        runs[n].background = NoColor;
                        runs[n].flags = (_scheme.bold & (1u << st)) != 0 ? TextCell::Bold : 0;
                        ++n;
                }
        }
        // The matched brackets: their own background, splitting a run they fall in.
        if (_bracketsShown) {
                for (const TextPosition &b : {_bracketA, _bracketB}) {
                        if (b.line != line) continue;
                        StyleRun mark;
                        mark.start = b.byte;
                        mark.end = b.byte + 1;
                        mark.text = _scheme.style[S(SyntaxStyle::Normal)];
                        mark.background = _scheme.bracket;
                        mark.flags = TextCell::Bold;
                        StyleRun out[TextEdit::MaxRuns];
                        size_t   t = 0;
                        bool     placed = false;
                        for (size_t k = 0; k < n && t + 3 <= max && t + 3 <= MaxRuns; ++k) {
                                const StyleRun r = runs[k];
                                if (!placed && b.byte < r.start) {
                                        out[t++] = mark;
                                        placed = true;
                                }
                                if (!placed && r.start <= b.byte && b.byte < r.end) {
                                        if (r.start < b.byte) {
                                                out[t] = r;
                                                out[t++].end = b.byte;
                                        }
                                        mark.text = r.text;
                                        out[t++] = mark;
                                        if (b.byte + 1 < r.end) {
                                                out[t] = r;
                                                out[t++].start = b.byte + 1;
                                        }
                                        placed = true;
                                        continue;
                                }
                                out[t++] = r;
                        }
                        if (!placed && t < max) out[t++] = mark;
                        for (size_t k = 0; k < t; ++k) runs[k] = out[k];
                        n = t;
                }
        }
        return n;
}

// ---- Brackets

SyntaxStyle CodeEdit::styleAt(uint32_t line, uint32_t byte) {
        if (_syntax == nullptr) return SyntaxStyle::Normal;
        StyleSpan      spans[TextEdit::MaxRuns];
        size_t         count = 0;
        const uint16_t start = startState(line);
        (void)_syntax->scan(document().line(line), start, spans, MaxRuns, count);
        for (size_t i = 0; i < count; ++i)
                if (spans[i].start <= byte && byte < spans[i].end) return spans[i].style;
        return SyntaxStyle::Normal;
}

// The bracket matching the one at @p at, brackets in strings, characters and comments not
// counted.
bool CodeEdit::findMatch(TextPosition at, TextPosition &match) {
        TextDocument &doc = document();
        const char    c = doc.byteAt(at);
        const char    closer = closerOf(c), opener = openerOf(c);
        if (closer == 0 && opener == 0) return false;
        const auto ignored = [&](uint32_t line, uint32_t byte) {
                const SyntaxStyle s = styleAt(line, byte);
                return s == SyntaxStyle::String || s == SyntaxStyle::Character || s == SyntaxStyle::Comment;
        };
        if (ignored(at.line, at.byte)) return false;
        const bool forward = closer != 0;
        const char want = forward ? closer : opener;
        int        depth = 0;
        uint32_t   line = at.line;
        int64_t    byte = int64_t(at.byte) + (forward ? 1 : -1);
        for (uint32_t seen = 0; seen < MatchLines; ++seen) {
                const StringView text = doc.line(line);
                while (byte >= 0 && byte < int64_t(text.size())) {
                        const char ch = text[size_t(byte)];
                        if ((ch == c || ch == want) && !ignored(line, uint32_t(byte))) {
                                if (ch == c) {
                                        ++depth;
                                } else if (depth == 0) {
                                        match = TextPosition{line, uint32_t(byte)};
                                        return true;
                                } else {
                                        --depth;
                                }
                        }
                        byte += forward ? 1 : -1;
                }
                if (forward) {
                        if (line + 1 >= doc.lineCount()) return false;
                        ++line;
                        byte = 0;
                } else {
                        if (line == 0) return false;
                        --line;
                        byte = int64_t(doc.line(line).size()) - 1;
                }
        }
        return false;
}

void CodeEdit::matchBrackets() {
        const bool         was = _bracketsShown;
        const TextPosition oldA = _bracketA, oldB = _bracketB;
        _bracketsShown = false;
        const TextSelection &sel = selection();
        if (!sel.hasSelection()) {
                // The bracket after the cursor, else the one before it.
                TextPosition p = sel.cursor, m;
                if (findMatch(p, m)) {
                        _bracketA = p;
                        _bracketB = m;
                        _bracketsShown = true;
                } else if (p.byte > 0 && findMatch(TextPosition{p.line, p.byte - 1}, m)) {
                        _bracketA = TextPosition{p.line, p.byte - 1};
                        _bracketB = m;
                        _bracketsShown = true;
                }
        }
        if (was) {
                updateLine(oldA.line);
                updateLine(oldB.line);
        }
        if (_bracketsShown) {
                updateLine(_bracketA.line);
                updateLine(_bracketB.line);
        }
        return;
}

void CodeEdit::cursorMovedHook() {
        matchBrackets();
        updateGutter(); // the current line's number
        return;
}

// ---- Indentation and comments

String CodeEdit::indentUnit() const {
        String unit(MemSpace::Psram);
        if ((_syntax != nullptr && _syntax->tabs) || !insertsSpaces()) {
                (void)unit.append('\t');
        } else {
                for (uint32_t i = 0; i < tabWidth(); ++i) (void)unit.append(' ');
        }
        return unit;
}

String CodeEdit::indentationOf(uint32_t line) const {
        const StringView text = document().line(line);
        size_t           n = 0;
        while (n < text.size() && isSpace(text[n])) ++n;
        String out(MemSpace::Psram);
        (void)out.assign(text.left(n));
        return out;
}

// Enter: the line's indentation (as far as the cursor), a level more after an opening bracket; a
// closing bracket right after the cursor goes to a line of its own, lined up with the line.
void CodeEdit::newline() {
        if (isReadOnly()) return;
        TextDocument    &doc = document();
        const TextRange  range = selection().range();
        const StringView text = doc.line(range.start.line);
        String           indent(MemSpace::Psram);
        if (autoIndents()) {
                size_t n = 0;
                while (n < text.size() && n < range.start.byte && isSpace(text[n])) ++n;
                (void)indent.assign(text.left(n));
        }
        uint32_t before = range.start.byte;
        while (before > 0 && isSpace(text[before - 1])) --before;
        const char       opened = before > 0 ? text[before - 1] : 0;
        const StringView rest = doc.line(range.end.line);
        const char       next = range.end.byte < rest.size() ? rest[range.end.byte] : 0;
        const bool       deeper = _syntax != nullptr && autoIndents() && closerOf(opened) != 0;
        String           insert(MemSpace::Psram);
        (void)insert.append('\n');
        (void)insert.append(indent.view());
        if (deeper) (void)insert.append(indentUnit().view());
        const size_t column = insert.size() - 1; // the cursor's byte on the new line
        if (deeper && next == closerOf(opened)) {
                (void)insert.append('\n');
                (void)insert.append(indent.view());
        }
        beginEdit();
        if (doc.replace(range, insert.view()).isOk())
                cursor().setPosition(TextPosition{range.start.line + 1, uint32_t(column)});
        endEdit();
        cursorChanged();
        return;
}

// A closing bracket typed first on its line: the line indented as its opening bracket's.
void CodeEdit::lineUpClosing() {
        TextDocument      &doc = document();
        const TextPosition p = selection().cursor;
        if (p.byte == 0) return;
        const StringView text = doc.line(p.line);
        for (uint32_t i = 0; i + 1 < p.byte; ++i)
                if (!isSpace(text[i])) return;
        TextPosition match;
        if (!findMatch(TextPosition{p.line, p.byte - 1}, match) || match.line == p.line) return;
        const String     want = indentationOf(match.line);
        const StringView have = text.left(p.byte - 1);
        if (have == want.view()) return;
        // Joined to the typing's step: undone with the bracket.
        beginEdit(Kind::Typing);
        (void)doc.replace(TextRange{{p.line, 0}, {p.line, p.byte - 1}}, want.view());
        cursor().setPosition(TextPosition{p.line, uint32_t(want.size() + 1)});
        endEdit();
        cursorChanged();
        return;
}

void CodeEdit::typed(StringView text) {
        if (_syntax != nullptr && autoIndents() && text.size() == 1 && openerOf(text[0]) != 0) lineUpClosing();
        return;
}

void CodeEdit::toggleComment() {
        if (isReadOnly() || _syntax == nullptr || _syntax->lineComment == nullptr) return;
        TextDocument   &doc = document();
        TextSelection   sel = selection();
        const TextRange range = sel.range();
        uint32_t        last = range.end.line;
        if (last > range.start.line && range.end.byte == 0) --last;
        const StringView mark(_syntax->lineComment);
        // Commented already if every non-blank line starts (after its indentation) with the mark.
        bool all = true;
        for (uint32_t line = range.start.line; line <= last && all; ++line) {
                const StringView t = doc.line(line);
                size_t           i = 0;
                while (i < t.size() && isSpace(t[i])) ++i;
                if (i < t.size() && !t.mid(i).startsWith(mark)) all = false;
        }
        // The mark goes at the least indentation among the lines, so a block stays lined up.
        size_t column = SIZE_MAX;
        for (uint32_t line = range.start.line; line <= last; ++line) {
                const StringView t = doc.line(line);
                size_t           i = 0;
                while (i < t.size() && isSpace(t[i])) ++i;
                if (i < t.size() && i < column) column = i;
        }
        if (column == SIZE_MAX) column = 0;
        String with(MemSpace::Psram);
        (void)with.append(mark);
        (void)with.append(' ');
        beginEdit();
        for (uint32_t line = range.start.line; line <= last; ++line) {
                const StringView t = doc.line(line);
                size_t           i = 0;
                while (i < t.size() && isSpace(t[i])) ++i;
                if (i == t.size()) continue; // blank
                int32_t  delta;
                uint32_t at;
                if (all) {
                        at = uint32_t(i);
                        size_t n = mark.size();
                        if (i + n < t.size() && t[i + n] == ' ') ++n;
                        (void)doc.remove(TextRange{{line, at}, {line, uint32_t(at + n)}});
                        delta = -int32_t(n);
                } else {
                        at = uint32_t(column);
                        (void)doc.insert(TextPosition{line, at}, with.view());
                        delta = int32_t(with.size());
                }
                for (TextPosition *p : {&sel.cursor, &sel.anchor}) {
                        if (p->line != line || p->byte < at) continue;
                        p->byte = delta > 0 ? p->byte + uint32_t(delta)
                                            : (p->byte - at > uint32_t(-delta) ? p->byte - uint32_t(-delta) : at);
                }
        }
        cursor().setSelection(sel);
        endEdit();
        cursorChanged();
        return;
}

bool CodeEdit::editKey(const KeyEvent &event) {
        const bool ctrl = event.hasCtrl() && !event.hasAlt();
        if (ctrl && !event.hasShift() && (event.usage == SlashKey || event.usage == HidKey::KeypadSlash)) {
                toggleComment();
                return true;
        }
        if (!ctrl && !event.hasAlt() && (event.usage == HidKey::Enter || event.usage == HidKey::KeypadEnter)) {
                newline();
                return true;
        }
        return false;
}

// ---- The gutter

uint32_t CodeEdit::digits() const {
        uint32_t n = document().lineCount(), d = 1;
        while (n >= 10) {
                n /= 10;
                ++d;
        }
        return d < 3 ? 3 : d;
}

uint16_t CodeEdit::markWidth() const {
        return _breakpointsEnabled ? BreakMarkWidth : MarkWidth;
}

uint16_t CodeEdit::gutterWidth() const {
        if (!_lineNumbers) return _diagnostics.isEmpty() && !_breakpointsEnabled ? 0 : markWidth() + 4;
        // The mark's column, the digits, a space; a multiple of 4 so the text stays on its column.
        return uint16_t(((markWidth() + (_digits + 1) * cellWidth()) + 3) & ~3u);
}

bool CodeEdit::hasDiagnostic(uint32_t line, Severity &worst) const {
        bool found = false;
        worst = Severity::Note;
        for (const Diagnostic &d : _diagnostics)
                if (d.line == line) {
                        if (!found || d.severity < worst) worst = d.severity;
                        found = true;
                }
        return found;
}

void CodeEdit::paintGutter(Painter &painter, uint32_t line, const Rect &row) {
        (void)painter.fill(row, _scheme.gutterBackground);
        if (line == UINT32_MAX) return;
        const int cy = row.y + row.height / 2; // the marks' middle
        if (hasBreakpoint(line)) {
                // A dot, 10 pixels across, as spans.
                static constexpr uint8_t Half[] = {2, 3, 4, 5, 5, 5, 5, 4, 3, 2};
                Rect                     spans[sizeof(Half)];
                for (size_t i = 0; i < sizeof(Half); ++i)
                        spans[i] = Rect{uint16_t(row.x + 7 - Half[i]), uint16_t(cy - 5 + int(i)), uint16_t(Half[i] * 2),
                                        1};
                (void)painter.fill(Span<const Rect>(spans, sizeof(Half)), _scheme.breakpoint);
        }
        if (line == _executionLine) {
                // An arrow: a shaft, then a head narrowing to the right.
                Rect spans[12];
                spans[0] = Rect{uint16_t(row.x + 2), uint16_t(cy - 2), 5, 4};
                for (int i = 0; i < 11; ++i) {
                        const int d = i - 5;
                        spans[i + 1] = Rect{uint16_t(row.x + 7), uint16_t(cy + d), uint16_t(6 - (d < 0 ? -d : d)), 1};
                }
                (void)painter.fill(Span<const Rect>(spans, 12), _scheme.executionMark);
        }
        Severity worst;
        if (hasDiagnostic(line, worst)) {
                const uint32_t color = worst == Severity::Error ? _scheme.error : _scheme.warning;
                const uint16_t x = uint16_t(row.x + (_breakpointsEnabled ? BreakMarkWidth - 3 : 2));
                (void)painter.fill(Rect{x, uint16_t(row.y + 4), uint16_t(_breakpointsEnabled ? 3 : 4), 8}, color);
        }
        if (!_lineNumbers) return;
        char           text[16];
        const int      n = std::snprintf(text, sizeof(text), "%u", unsigned(line + 1));
        const uint16_t width = uint16_t(n * cellWidth());
        const uint16_t right = uint16_t(row.x + row.width - cellWidth() / 2);
        int            x = int(right) - width;
        x &= ~3;
        const bool current = line == selection().cursor.line;
        (void)painter.text(x, row.y, StringView(text, size_t(n)), Fonts::unscii16,
                           current ? _scheme.gutterCurrent : _scheme.gutterText, _scheme.gutterBackground,
                           uint16_t(row.x + row.width - x));
        return;
}

uint32_t CodeEdit::lineBackground(uint32_t line) const {
        if (line == _executionLine) return _scheme.execution;
        if (line == selection().cursor.line && !selection().hasSelection()) return _scheme.currentLine;
        return _scheme.background;
}

// ---- Breakpoints and the line about to run

void CodeEdit::setBreakpointsEnabled(bool on) {
        if (on == _breakpointsEnabled) return;
        _breakpointsEnabled = on;
        relayoutContent();
        return;
}

bool CodeEdit::hasBreakpoint(uint32_t line) const {
        for (uint32_t b : _breakpoints)
                if (b == line) return true;
        return false;
}

bool CodeEdit::toggleBreakpoint(uint32_t line) {
        size_t at = 0;
        while (at < _breakpoints.size() && _breakpoints[at] < line) ++at;
        bool set = false;
        if (at < _breakpoints.size() && _breakpoints[at] == line) {
                _breakpoints.removeAt(at);
        } else {
                if (_breakpoints.insert(at, uint32_t(line)).isError()) return false;
                set = true;
        }
        updateLine(line);
        tellBreakpoints();
        return set;
}

void CodeEdit::clearBreakpoints() {
        if (_breakpoints.isEmpty()) return;
        _breakpoints.clear();
        updateGutter();
        tellBreakpoints();
        return;
}

void CodeEdit::tellBreakpoints() {
        if (_breakpointsChanged != nullptr) _breakpointsChanged(_breakpointsChangedArg, *this);
        return;
}

void CodeEdit::setExecutionLine(uint32_t line) {
        if (line == _executionLine) return;
        const uint32_t was = _executionLine;
        _executionLine = line;
        if (was != NoLine) updateLine(was);
        if (line != NoLine) updateLine(line);
        return;
}

void CodeEdit::gutterClicked(uint32_t line, const WindowManager::MouseEvent &event) {
        if (!_breakpointsEnabled) {
                TextEdit::gutterClicked(line, event);
                return;
        }
        if (line < document().lineCount()) (void)toggleBreakpoint(line);
        return;
}

// ---- Diagnostics

Error CodeEdit::setDiagnostics(Span<const Diagnostic> diagnostics) {
        const bool hadGutterChange = _diagnostics.isEmpty() != diagnostics.isEmpty() && !_lineNumbers;
        _diagnostics.clear();
        Error err = Error::Ok;
        for (const Diagnostic &d : diagnostics) {
                Diagnostic copy;
                copy.line = d.line;
                copy.start = d.start;
                copy.end = d.end;
                copy.severity = d.severity;
                err = copy.message.assign(d.message.view());
                if (err.isOk()) err = _diagnostics.append(std::move(copy));
                if (err.isError()) break;
        }
        if (hadGutterChange)
                relayoutContent();
        else
                update();
        GuiWindow *w = window();
        if (w != nullptr) w->application().toolTipChanged(this);
        return err;
}

const CodeEdit::Diagnostic *CodeEdit::diagnosticAt(TextPosition position) const {
        for (const Diagnostic &d : _diagnostics) {
                if (d.line != position.line) continue;
                uint32_t end = d.end > d.start ? d.end : d.start + 1;
                if (position.byte >= d.start && position.byte < end) return &d;
        }
        return nullptr;
}

const CodeEdit::Diagnostic *CodeEdit::nextDiagnostic(bool backward) {
        if (_diagnostics.isEmpty()) return nullptr;
        const TextPosition at = selection().cursor;
        const Diagnostic  *best = nullptr, *wrap = nullptr;
        for (const Diagnostic &d : _diagnostics) {
                const TextPosition p{d.line, d.start};
                if (!backward) {
                        if (p > at && (best == nullptr || p < TextPosition{best->line, best->start})) best = &d;
                        if (wrap == nullptr || p < TextPosition{wrap->line, wrap->start}) wrap = &d;
                } else {
                        if (p < at && (best == nullptr || p > TextPosition{best->line, best->start})) best = &d;
                        if (wrap == nullptr || p > TextPosition{wrap->line, wrap->start}) wrap = &d;
                }
        }
        const Diagnostic *d = best != nullptr ? best : wrap;
        setSelection(TextSelection{TextPosition{d->line, d->start}, TextPosition{d->line, d->start}});
        return d;
}

// The wavy underline: two-pixel steps up and down under each diagnostic's columns.
void CodeEdit::paintOver(Painter &painter, uint32_t line, int y, const Rect &area) {
        for (const Diagnostic &d : _diagnostics) {
                if (d.line != line) continue;
                const TextDocument &doc = document();
                const StringView    text = doc.line(line);
                uint32_t            end = d.end;
                if (end <= d.start) {
                        // To the end of the word there, or of the string (one character at least).
                        end = d.start;
                        const char quote = d.start < text.size() ? text[d.start] : 0;
                        if (quote == '"' || quote == '\'') {
                                end = d.start + 1;
                                while (end < text.size() && text[end] != quote) end += text[end] == '\\' ? 2 : 1;
                                end = end < text.size() ? end + 1 : uint32_t(text.size());
                        }
                        while (end < text.size() && TextDocument::isWordByte(text[end])) ++end;
                        if (end == d.start) end = d.start + 1;
                }
                const int x0 = columnX(doc.columnOf(line, d.start, tabWidth()));
                const int x1 = columnX(doc.columnOf(line, end, tabWidth())) + (end > text.size() ? cellWidth() : 0);
                const uint32_t color = d.severity == Severity::Error ? _scheme.error : _scheme.warning;
                for (int x = x0 < int(area.x) ? int(area.x) : x0; x < x1 && x < int(area.right()); x += 2) {
                        const int up = ((x - x0) / 2) % 2;
                        (void)painter.fill(Rect{uint16_t(x), uint16_t(y + LineHeight - 2 - up), 2, 1}, color);
                }
        }
        return;
}

// ---- The tooltip: the message of the diagnostic under the pointer.

void CodeEdit::mouseMoved(const WindowManager::MouseEvent &event) {
        TextEdit::mouseMoved(event);
        if (_diagnostics.isEmpty()) return;
        // Where the pointer is, without moving anything (TextEdit's own mapping).
        const Rect v = viewport();
        if (event.y < int(v.y) || event.y >= int(v.bottom())) return;
        const uint32_t     line = uint32_t((event.y - v.y + int(scrollY())) / LineHeight);
        const int          dx = event.x - columnX(0);
        const uint32_t     column = dx < 0 ? 0 : uint32_t(dx / cellWidth());
        const TextPosition at{line, document().byteAtColumn(line, column, tabWidth())};
        const Diagnostic  *before = _hovering ? diagnosticAt(_hover) : nullptr;
        _hover = at;
        _hovering = true;
        const Diagnostic *now = diagnosticAt(_hover);
        GuiWindow        *w = window();
        if (now != before && w != nullptr) w->application().toolTipChanged(this);
        return;
}

StringView CodeEdit::toolTip() const {
        if (!_hovering) return StringView();
        const Diagnostic *d = diagnosticAt(_hover);
        return d != nullptr ? d->message.view() : StringView();
}

BMDS_NAMESPACE_END
