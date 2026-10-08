/**
 * @file      lineeditor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <bmds/console.h>
#include <bmds/lineeditor.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char ctrl(char c) {
                return static_cast<char>(c & 0x1f);
        }

        constexpr char Esc = '\x1b';
        constexpr char Del = '\x7f';

        constexpr unsigned ParamMax = 9999;

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

} // namespace

// Collects echo output so one key press is one (or a few) console writes.
class LineEditor::Output {
        public:
                explicit Output(Console &console) : _console(console) {}
                ~Output() { flush(); }

                Output(const Output &) = delete;
                Output &operator=(const Output &) = delete;

                void put(char c) {
                        if (_used == sizeof(_buf)) flush();
                        _buf[_used++] = c;
                        return;
                }

                void put(StringView text) {
                        for (char c : text) put(c);
                        return;
                }

                // ESC [ n <final>, omitted when n is 0.
                void csi(size_t n, char final) {
                        if (n == 0) return;
                        char      seq[16];
                        const int len = std::snprintf(seq, sizeof(seq), "\x1b[%u%c", static_cast<unsigned>(n), final);
                        if (len > 0) put(StringView(seq, static_cast<size_t>(len)));
                        return;
                }

                void flush() {
                        if (_used == 0) return;
                        (void)_console.write(StringView(_buf, _used), OutputTimeoutMs);
                        _used = 0;
                        return;
                }

        private:
                Console &_console;
                size_t   _used = 0;
                char     _buf[96];
};

LineEditor::LineEditor(Console &console) : _console(console) {}

void LineEditor::start(StringView prompt) {
        _prompt = prompt;
        _length = 0;
        _cursor = 0;
        _screenCursor = 0;
        _historyPos = 0;
        _state = InputState::Normal;
        _overflowed = false;
        _line[0] = '\0';
        Output out(_console);
        out.put(_prompt);
        return;
}

LineEditor::Event LineEditor::feed(char c) {
        Output     out(_console);
        const bool wasCr = _lastWasCr;
        _lastWasCr = (c == '\r');

        switch (_state) {
                case InputState::Normal: break;

                case InputState::Esc:
                        _state = InputState::Normal;
                        switch (c) {
                                case '[':
                                        _state = InputState::Csi;
                                        _params[0] = _params[1] = 0;
                                        _paramCount = 0;
                                        break;
                                case 'O': _state = InputState::Ss3; break;
                                case 'b': moveTo(wordLeft(), out); break;
                                case 'f': moveTo(wordRight(), out); break;
                                case Del: eraseRange(wordLeft(), _cursor, out); break;
                                default: break;
                        }
                        return Event::None;

                case InputState::Csi:
                        if (c >= '0' && c <= '9') {
                                if (_paramCount == 0) _paramCount = 1;
                                unsigned &p = _params[_paramCount - 1];
                                p = p * 10 + static_cast<unsigned>(c - '0');
                                if (p > ParamMax) p = ParamMax;
                        } else if (c == ';') {
                                if (_paramCount == 0) _paramCount = 1;
                                if (_paramCount < 2) ++_paramCount;
                        } else if (c >= 0x40 && c <= 0x7e) {
                                _state = InputState::Normal;
                                handleSequence(c, out);
                        } else if (c < 0x20 || c > 0x7e) {
                                _state = InputState::Normal; // malformed; drop it
                        }
                        return Event::None;

                case InputState::Ss3:
                        _state = InputState::Normal;
                        _params[0] = _params[1] = 0;
                        _paramCount = 0;
                        handleSequence(c, out);
                        return Event::None;
        }

        if (c == '\n' && wasCr) return Event::None; // the LF of a CR LF pair
        const uint8_t u = static_cast<uint8_t>(c);
        if (u >= 0x80) {
                feedUtf8(u, out);
                return Event::None;
        }
        _utf8Need = 0; // an ASCII byte ends any partial character
        if (c >= 0x20 && c < Del) {
                insert(c, out);
                return Event::None;
        }
        return handleControl(c, out);
}

LineEditor::Event LineEditor::handleControl(char c, Output &out) {
        switch (c) {
                case '\r':
                case '\n':
                        out.put('\n');
                        _line[_length] = '\0';
                        // Some of it was refused: what is left isn't what was typed (or pasted).
                        if (_overflowed) return Event::TooLong;
                        addHistory(line());
                        _historyPos = 0;
                        return Event::Line;

                case Esc: _state = InputState::Esc; break;

                case Del:
                case ctrl('H'):
                        if (_cursor > 0) eraseRange(previousCharacter(_cursor), _cursor, out);
                        break;

                case ctrl('D'):
                        if (_length == 0) return Event::Eof;
                        if (_cursor < _length) eraseRange(_cursor, nextCharacter(_cursor), out);
                        break;

                case ctrl('C'):
                        out.put("^C\n");
                        _overflowed = false;
                        _length = 0;
                        _cursor = 0;
                        _line[0] = '\0';
                        return Event::Cancel;

                case ctrl('A'): moveTo(0, out); break;
                case ctrl('E'): moveTo(_length, out); break;
                case ctrl('B'):
                        if (_cursor > 0) moveTo(previousCharacter(_cursor), out);
                        break;
                case ctrl('F'):
                        if (_cursor < _length) moveTo(nextCharacter(_cursor), out);
                        break;
                case ctrl('P'): historyUp(out); break;
                case ctrl('N'): historyDown(out); break;
                case ctrl('K'): eraseRange(_cursor, _length, out); break;
                case ctrl('U'): eraseRange(0, _cursor, out); break;
                case ctrl('W'): eraseRange(wordLeft(), _cursor, out); break;

                case ctrl('L'):
                        out.put("\x1b[H\x1b[2J");
                        out.put(_prompt);
                        out.put(line());
                        out.csi(columns(_cursor, _length), 'D');
                        _screenCursor = _cursor;
                        break;

                case '\t': complete(out); break;

                default: break; // other control bytes
        }
        return Event::None;
}

void LineEditor::handleSequence(char final, Output &out) {
        // xterm sends modifiers as a second parameter: ESC[1;5C is Ctrl-Right,
        // ESC[1;3C is Alt-Right.
        const bool word = _paramCount == 2 && (_params[1] == 3 || _params[1] == 5);
        switch (final) {
                case 'A': historyUp(out); break;
                case 'B': historyDown(out); break;
                case 'C':
                        if (word) {
                                moveTo(wordRight(), out);
                        } else if (_cursor < _length) {
                                moveTo(nextCharacter(_cursor), out);
                        }
                        break;
                case 'D':
                        if (word) {
                                moveTo(wordLeft(), out);
                        } else if (_cursor > 0) {
                                moveTo(previousCharacter(_cursor), out);
                        }
                        break;
                case 'H': moveTo(0, out); break;
                case 'F': moveTo(_length, out); break;
                case '~':
                        switch (_params[0]) {
                                case 1:
                                case 7: moveTo(0, out); break;
                                case 4:
                                case 8: moveTo(_length, out); break;
                                case 3:
                                        if (_cursor < _length) eraseRange(_cursor, nextCharacter(_cursor), out);
                                        break;
                                default: break;
                        }
                        break;
                default: break;
        }
        return;
}

void LineEditor::insert(char c, Output &out) {
        insertBytes(&c, 1, out);
        return;
}

void LineEditor::insertBytes(const char *bytes, size_t n, Output &out) {
        if (_length + n > LineMax) {
                out.put('\a');
                _overflowed = true;
                return;
        }
        std::memmove(_line + _cursor + n, _line + _cursor, _length - _cursor);
        std::memcpy(_line + _cursor, bytes, n);
        _length += n;
        _cursor += n;
        _line[_length] = '\0';
        if (_cursor == _length) {
                // Typing at the end of the line: just echo.
                out.put(StringView(bytes, n));
                _screenCursor = _cursor;
        } else {
                refreshFrom(_cursor - n, out);
        }
        return;
}

// Collects the bytes of a UTF-8 character, then inserts it whole.
void LineEditor::feedUtf8(uint8_t b, Output &out) {
        if (!Utf8::isContinuation(b)) {
                _utf8Need = b >= 0xF0 ? 3 : (b >= 0xE0 ? 2 : (b >= 0xC0 ? 1 : 0));
                _utf8Length = 0;
                if (_utf8Need == 0 || b > 0xF4) {
                        _utf8Need = 0; // not a lead byte
                        return;
                }
        } else if (_utf8Need == 0) {
                return; // a stray continuation byte
        } else {
                --_utf8Need;
        }
        _utf8[_utf8Length++] = static_cast<char>(b);
        if (_utf8Need == 0 && _utf8Length > 1) insertBytes(_utf8, _utf8Length, out);
        return;
}

size_t LineEditor::columns(size_t from, size_t to) const {
        return Utf8::displayWidth(StringView(_line + from, to - from));
}

size_t LineEditor::previousCharacter(size_t pos) const {
        if (pos == 0) return 0;
        --pos;
        while (pos > 0 && Utf8::isContinuation(static_cast<uint8_t>(_line[pos]))) --pos;
        return pos;
}

size_t LineEditor::nextCharacter(size_t pos) const {
        if (pos >= _length) return _length;
        ++pos;
        while (pos < _length && Utf8::isContinuation(static_cast<uint8_t>(_line[pos]))) ++pos;
        return pos;
}

void LineEditor::eraseRange(size_t from, size_t to, Output &out) {
        if (from >= to) return;
        // Move the terminal cursor while the columns before it are still there.
        moveScreenTo(from, out);
        std::memmove(_line + from, _line + to, _length - to);
        _length -= to - from;
        _line[_length] = '\0';
        if (_cursor >= to) {
                _cursor -= to - from;
        } else if (_cursor > from) {
                _cursor = from;
        }
        refreshFrom(from, out);
        return;
}

void LineEditor::moveScreenTo(size_t pos, Output &out) {
        if (pos > _screenCursor) {
                out.csi(columns(_screenCursor, pos), 'C');
        } else {
                out.csi(columns(pos, _screenCursor), 'D');
        }
        _screenCursor = pos;
        return;
}

void LineEditor::moveTo(size_t pos, Output &out) {
        if (pos > _screenCursor) {
                out.csi(columns(_screenCursor, pos), 'C');
        } else {
                out.csi(columns(pos, _screenCursor), 'D');
        }
        _cursor = pos;
        _screenCursor = pos;
        return;
}

void LineEditor::refreshFrom(size_t from, Output &out) {
        // Move to `from`, rewrite the rest of the line, clear what was left
        // over from a longer line, then put the cursor back where it belongs.
        if (from < _screenCursor) {
                out.csi(columns(from, _screenCursor), 'D');
        } else {
                out.csi(columns(_screenCursor, from), 'C');
        }
        out.put(StringView(_line + from, _length - from));
        out.put("\x1b[K");
        out.csi(columns(_cursor, _length), 'D');
        _screenCursor = _cursor;
        return;
}

void LineEditor::loadLine(StringView text, Output &out) {
        moveScreenTo(0, out); // measured on the old line
        const size_t len = text.size() < LineMax ? text.size() : LineMax;
        std::memcpy(_line, text.data(), len);
        _length = len;
        _line[_length] = '\0';
        _cursor = _length;
        refreshFrom(0, out);
        return;
}

void LineEditor::historyUp(Output &out) {
        if (_historyPos >= historyCount()) return;
        if (_historyPos == 0) {
                std::memcpy(_saved, _line, _length + 1);
                _savedLength = _length;
        }
        ++_historyPos;
        loadLine(historyEntry(_historyPos - 1), out);
        return;
}

void LineEditor::historyDown(Output &out) {
        if (_historyPos == 0) return;
        --_historyPos;
        loadLine(_historyPos == 0 ? StringView(_saved, _savedLength) : historyEntry(_historyPos - 1), out);
        return;
}

size_t LineEditor::wordLeft() const {
        size_t pos = _cursor;
        while (pos > 0 && isSpace(_line[pos - 1])) --pos;
        while (pos > 0 && !isSpace(_line[pos - 1])) --pos;
        return pos;
}

size_t LineEditor::wordRight() const {
        size_t pos = _cursor;
        while (pos < _length && isSpace(_line[pos])) ++pos;
        while (pos < _length && !isSpace(_line[pos])) ++pos;
        return pos;
}

size_t LineEditor::historyCount() const {
        size_t count = 0;
        for (size_t i = 0; i < _historyUsed; ++i) {
                if (_history[i] == '\0') ++count;
        }
        return count;
}

StringView LineEditor::historyEntry(size_t index) const {
        // Entries are packed oldest first, each NUL-terminated; walk back from the end.
        size_t end = _historyUsed;
        for (;;) {
                if (end == 0) return StringView();
                size_t begin = end - 1; // index of this entry's NUL
                while (begin > 0 && _history[begin - 1] != '\0') --begin;
                if (index == 0) return StringView(_history + begin, end - 1 - begin);
                --index;
                end = begin;
        }
}

void LineEditor::addHistory(StringView text) {
        if (text.isEmpty() || text.size() + 1 > HistoryBytes) return;
        if (historyCount() > 0 && historyEntry(0) == text) return;
        const size_t need = text.size() + 1;
        while (HistoryBytes - _historyUsed < need) {
                // Evict the oldest entry.
                size_t first = 0;
                while (_history[first] != '\0') ++first;
                ++first;
                std::memmove(_history, _history + first, _historyUsed - first);
                _historyUsed -= first;
        }
        std::memcpy(_history + _historyUsed, text.data(), text.size());
        _history[_historyUsed + text.size()] = '\0';
        _historyUsed += need;
        return;
}

namespace {

        bool isWordBreak(char c) {
                return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' || c == '<' || c == '>' || c == '(' ||
                       c == ')';
        }

        // Counts candidates for a word and narrows their common prefix.
        class PrefixSink final : public CompletionSink {
                public:
                        explicit PrefixSink(StringView word) : _word(word) {}

                        void add(StringView candidate) override {
                                if (!candidate.startsWith(_word) || candidate.size() > LineEditor::LineMax) return;
                                if (_count++ == 0) {
                                        for (size_t i = 0; i < candidate.size(); ++i) _common[i] = candidate[i];
                                        _commonLength = candidate.size();
                                } else {
                                        size_t n = 0;
                                        while (n < _commonLength && n < candidate.size() && _common[n] == candidate[n])
                                                ++n;
                                        _commonLength = n;
                                }
                                if (candidate.size() > _longest) _longest = candidate.size();
                                return;
                        }

                        size_t     count() const { return _count; }
                        size_t     longest() const { return _longest; }
                        StringView common() const { return StringView(_common, _commonLength); }

                private:
                        StringView _word;
                        size_t     _count = 0;
                        size_t     _longest = 0;
                        size_t     _commonLength = 0;
                        char       _common[LineEditor::LineMax];
        };

} // namespace

void LineEditor::complete(Output &out) {
        if (_completer == nullptr) return;
        // The word: back to a break that isn't escaped (`my\ file`).
        size_t start = _cursor;
        while (start > 0 && (!isWordBreak(_line[start - 1]) || (start > 1 && _line[start - 2] == '\\'))) --start;
        const StringView before(_line, _cursor);
        const StringView word = before.mid(start);
        PrefixSink       prefix(word);
        _completer->complete(before, start, prefix);
        if (prefix.count() == 0) {
                out.put('\a');
                return;
        }
        const StringView common = prefix.common();
        for (size_t i = word.size(); i < common.size(); ++i) insert(common[i], out);
        if (prefix.count() == 1) {
                // A directory ends in '/', ready for the next name.
                if (!common.endsWith('/')) insert(' ', out);
                return;
        }
        if (common.size() > word.size()) return;
        // Ambiguous and nothing to add: list them in columns, then the line
        // again. Paths are listed by their last component, as bash does.
        class ListSink final : public CompletionSink {
                public:
                        ListSink(StringView w, size_t strip, size_t width, Output &o)
                            : _word(w), _strip(strip), _width(width), _out(o),
                              _perRow(80 / width > 0 ? 80 / width : 1) {} // 80 columns

                        void add(StringView candidate) override {
                                if (!candidate.startsWith(_word)) return;
                                candidate = candidate.mid(_strip);
                                _out.put(candidate);
                                if (++_column == _perRow) {
                                        _out.put('\n');
                                        _column = 0;
                                } else {
                                        for (size_t i = candidate.size(); i < _width; ++i) _out.put(' ');
                                }
                                return;
                        }

                        bool rowOpen() const { return _column != 0; }

                private:
                        StringView _word;
                        size_t     _strip;
                        size_t     _width;
                        Output    &_out;
                        size_t     _perRow;
                        size_t     _column = 0;
        };
        out.put('\n');
        const size_t slash = word.lastIndexOf('/');
        const size_t strip = slash == StringView::NPos ? 0 : slash + 1;
        ListSink     list(word, strip, prefix.longest() - strip + 2, out);
        _completer->complete(before, start, list);
        if (list.rowOpen()) out.put('\n');
        out.put(_prompt);
        out.put(line());
        out.csi(columns(_cursor, _length), 'D');
        _screenCursor = _cursor;
        return;
}

void LineEditor::redraw() {
        Output out(_console);
        out.put('\r');
        out.put(_prompt);
        out.put(line());
        out.put("\x1b[K");
        out.csi(columns(_cursor, _length), 'D');
        _screenCursor = _cursor;
        return;
}

BMDS_NAMESPACE_END
