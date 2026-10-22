/**
 * @file      textdocument.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/textdocument.h>

#include <cstring>
#include <bmds/memory.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   NoStep = static_cast<size_t>(-1);
        constexpr uint32_t MinLineCapacity = 16;

        char lower(char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

        // A character's kind for moving by words: a word's, a space, or punctuation.
        enum class Kind : uint8_t {
                Word,
                Space,
                Punct
        };

        Kind kindOf(char c) {
                if (TextDocument::isWordByte(c)) return Kind::Word;
                if (isSpace(c)) return Kind::Space;
                return Kind::Punct;
        }

        // @p text with CR LF read as LF, into @p storage when it has any (else @p text itself).
        Result<StringView> normalized(StringView text, String &storage) {
                if (text.indexOf("\r\n") == StringView::NPos) return Result<StringView>(text, Error::Ok);
                Error err = storage.reserve(text.size());
                for (size_t i = 0; err.isOk() && i < text.size(); ++i) {
                        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
                        err = storage.append(text[i]);
                }
                return Result<StringView>(storage.view(), err);
        }

} // namespace

TextDocument::TextDocument(MemSpace space) : _space(space) {
        (void)resetToEmpty();
}

TextDocument::~TextDocument() {
        freeAll();
        Memory::free(_lines);
}

// ---- Lines

// Room for @p count lines: doubling as edits add them, or just that many (a load, which knows).
Error TextDocument::reserveLines(uint32_t count, bool exact) {
        if (count <= _capacity) return Error::Ok;
        uint32_t capacity = _capacity != 0 ? _capacity : 64;
        while (capacity < count) capacity *= 2;
        if (exact && count > 64) capacity = count;
        auto *grown = static_cast<Line *>(Memory::alloc(_space, sizeof(Line) * capacity));
        if (grown == nullptr) return Error::NoMem;
        if (_count != 0) std::memcpy(static_cast<void *>(grown), _lines, sizeof(Line) * _count);
        Memory::free(_lines);
        _lines = grown;
        _capacity = capacity;
        return Error::Ok;
}

// Room for @p length bytes: a line in the loaded block may shrink there, but grows into a block
// of its own.
Error TextDocument::fitLine(Line &line, size_t length) {
        if (length <= line.capacity || (line.capacity == 0 && line.data != nullptr && length <= line.length))
                return Error::Ok;
        if (length > UINT32_MAX / 2) return Error::NoMem;
        uint32_t capacity = line.capacity > MinLineCapacity ? line.capacity : MinLineCapacity;
        while (capacity < length) capacity *= 2;
        auto *grown = static_cast<char *>(Memory::alloc(_space, capacity));
        if (grown == nullptr) return Error::NoMem;
        if (line.length != 0) std::memcpy(grown, line.data, line.length);
        if (line.capacity != 0) Memory::free(line.data);
        line.data = grown;
        line.capacity = capacity;
        return Error::Ok;
}

void TextDocument::freeLine(Line &line) {
        if (line.capacity != 0) Memory::free(line.data);
        line = Line{};
        return;
}

void TextDocument::freeAll() {
        for (uint32_t i = 0; i < _count; ++i) freeLine(_lines[i]);
        Memory::free(_loaded);
        _loaded = nullptr;
        _count = 0;
        _length = 0;
        return;
}

Error TextDocument::resetToEmpty() {
        freeAll();
        Error err = reserveLines(1);
        if (err.isError()) return err;
        _lines[0] = Line{};
        _count = 1;
        return Error::Ok;
}

// ---- Loading and saving

Error TextDocument::load(StringView bytes) {
        const uint32_t oldCount = _count;
        clearUndo();
        _bom = bytes.startsWith("\xEF\xBB\xBF");
        if (_bom) bytes = bytes.mid(3);
        // Count the lines first, so the array is made once.
        uint32_t lines = 1;
        for (char c : bytes)
                if (c == '\n') ++lines;
        freeAll();
        if (_capacity > lines + 64) { // a smaller file than the last: the array made again
                Memory::free(_lines);
                _lines = nullptr;
                _capacity = 0;
        }
        Error err = reserveLines(lines, true);
        if (err.isOk() && !bytes.isEmpty()) {
                _loaded = static_cast<char *>(Memory::alloc(_space, bytes.size()));
                if (_loaded == nullptr)
                        err = Error::NoMem;
                else
                        std::memcpy(_loaded, bytes.data(), bytes.size());
        }
        _ending = LineEnding::Lf;
        bool   endingFound = false;
        size_t start = 0;
        while (err.isOk()) {
                size_t     nl = bytes.indexOf('\n', start);
                const bool last = nl == StringView::NPos;
                size_t     end = last ? bytes.size() : nl;
                Line       line;
                if (!last && end > start && bytes[end - 1] == '\r') {
                        line.crlf = true;
                        --end;
                }
                if (!last && !endingFound) {
                        _ending = line.crlf ? LineEnding::CrLf : LineEnding::Lf;
                        endingFound = true;
                }
                // A part of the loaded block (capacity 0: not its own).
                line.data = end > start ? _loaded + start : nullptr;
                line.length = static_cast<uint32_t>(end - start);
                _lines[_count++] = line;
                _length += line.length + (last ? 0 : 1);
                if (last) break;
                start = nl + 1;
        }
        if (err.isError()) (void)resetToEmpty();
        _step = 0;
        _savedStep = 0;
        notify(Change{0, oldCount, _count});
        return err;
}

size_t TextDocument::savedSize() const {
        size_t size = _bom ? 3 : 0;
        for (uint32_t i = 0; i < _count; ++i) {
                size += _lines[i].length;
                if (i + 1 < _count) size += _lines[i].crlf ? 2 : 1;
        }
        return size;
}

Error TextDocument::save(String &out) const {
        out.clear();
        Error err = out.reserve(savedSize());
        if (err.isOk() && _bom) err = out.append("\xEF\xBB\xBF");
        for (uint32_t i = 0; err.isOk() && i < _count; ++i) {
                err = out.append(StringView(_lines[i].data, _lines[i].length));
                if (err.isOk() && i + 1 < _count)
                        err = out.append(_lines[i].crlf ? StringView("\r\n") : StringView("\n"));
        }
        return err;
}

void TextDocument::setSaved() {
        _savedStep = _step;
        _mergeable = false;
        return;
}

void TextDocument::setLineEnding(LineEnding ending, bool all) {
        _ending = ending;
        if (!all) return;
        bool changed = false;
        for (uint32_t i = 0; i + 1 < _count; ++i) {
                const bool crlf = ending == LineEnding::CrLf;
                if (_lines[i].crlf != crlf) changed = true;
                _lines[i].crlf = crlf;
        }
        if (changed) _savedStep = NoStep;
        return;
}

// ---- Reading

StringView TextDocument::line(uint32_t index) const {
        if (index >= _count) return StringView();
        return StringView(_lines[index].data, _lines[index].length);
}

TextDocument::LineEnding TextDocument::lineEndingOf(uint32_t index) const {
        if (index >= _count) return _ending;
        return _lines[index].crlf ? LineEnding::CrLf : LineEnding::Lf;
}

TextPosition TextDocument::end() const {
        return TextPosition{_count - 1, _lines[_count - 1].length};
}

TextPosition TextDocument::clamp(TextPosition p) const {
        if (p.line >= _count) return end();
        if (p.byte > _lines[p.line].length) p.byte = _lines[p.line].length;
        return p;
}

Error TextDocument::text(TextRange range, String &out) const {
        out.clear();
        range.start = clamp(range.start);
        range.end = clamp(range.end);
        if (range.end < range.start) return Error::Ok;
        Error err = Error::Ok;
        for (uint32_t i = range.start.line; err.isOk() && i <= range.end.line; ++i) {
                const uint32_t from = i == range.start.line ? range.start.byte : 0;
                const uint32_t to = i == range.end.line ? range.end.byte : _lines[i].length;
                err = out.append(StringView(_lines[i].data + from, to - from));
                if (err.isOk() && i != range.end.line) err = out.append('\n');
        }
        return err;
}

char TextDocument::byteAt(TextPosition p) const {
        if (p.line >= _count) return 0;
        const Line &l = _lines[p.line];
        if (p.byte < l.length) return l.data[p.byte];
        return p.line + 1 < _count ? '\n' : 0;
}

uint16_t TextDocument::lineState(uint32_t index) const {
        return index < _count ? _lines[index].state : 0;
}

void TextDocument::setLineState(uint32_t index, uint16_t state) {
        if (index < _count) _lines[index].state = state;
        return;
}

// ---- Moving

bool TextDocument::isWordByte(char c) {
        const auto u = static_cast<uint8_t>(c);
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u >= 0x80;
}

uint32_t TextDocument::characterColumns(uint32_t codepoint) {
        if (codepoint < 0x20 || codepoint == 0x7F || codepoint == Utf8::Replacement) return 1;
        return static_cast<uint32_t>(Utf8::displayWidth(codepoint));
}

TextPosition TextDocument::nextCharacter(TextPosition p) const {
        p = clamp(p);
        const Line &l = _lines[p.line];
        if (p.byte < l.length) {
                size_t i = p.byte;
                (void)Utf8::decode(StringView(l.data, l.length), i);
                return TextPosition{p.line, static_cast<uint32_t>(i)};
        }
        if (p.line + 1 < _count) return TextPosition{p.line + 1, 0};
        return p;
}

TextPosition TextDocument::previousCharacter(TextPosition p) const {
        p = clamp(p);
        if (p.byte == 0) {
                if (p.line == 0) return p;
                return TextPosition{p.line - 1, _lines[p.line - 1].length};
        }
        const Line &l = _lines[p.line];
        uint32_t    i = p.byte - 1;
        // Back over continuation bytes, at most three, to a lead byte (or a stray byte alone).
        for (int n = 0; n < 3 && i > 0 && Utf8::isContinuation(static_cast<uint8_t>(l.data[i])); ++n) --i;
        size_t check = i;
        (void)Utf8::decode(StringView(l.data, l.length), check);
        if (check != p.byte) i = p.byte - 1; // not a whole character: one byte back
        return TextPosition{p.line, i};
}

TextPosition TextDocument::nextWord(TextPosition p) const {
        p = clamp(p);
        const Line &l = _lines[p.line];
        if (p.byte >= l.length) return nextCharacter(p);
        uint32_t   i = p.byte;
        const Kind k = kindOf(l.data[i]);
        if (k != Kind::Space)
                while (i < l.length && kindOf(l.data[i]) == k) ++i;
        while (i < l.length && isSpace(l.data[i])) ++i;
        return TextPosition{p.line, i};
}

TextPosition TextDocument::previousWord(TextPosition p) const {
        p = clamp(p);
        if (p.byte == 0) return previousCharacter(p);
        const Line &l = _lines[p.line];
        uint32_t    i = p.byte;
        while (i > 0 && isSpace(l.data[i - 1])) --i;
        if (i == 0) return TextPosition{p.line, 0};
        const Kind k = kindOf(l.data[i - 1]);
        while (i > 0 && kindOf(l.data[i - 1]) == k) --i;
        return TextPosition{p.line, i};
}

TextRange TextDocument::wordAt(TextPosition p) const {
        p = clamp(p);
        const Line &l = _lines[p.line];
        uint32_t    at = p.byte;
        if (!(at < l.length && isWordByte(l.data[at]))) {
                if (at > 0 && isWordByte(l.data[at - 1]))
                        --at;
                else
                        return TextRange{p, p};
        }
        uint32_t start = at, end = at;
        while (start > 0 && isWordByte(l.data[start - 1])) --start;
        while (end < l.length && isWordByte(l.data[end])) ++end;
        return TextRange{{p.line, start}, {p.line, end}};
}

uint32_t TextDocument::columnOf(uint32_t index, uint32_t byte, uint32_t tabWidth) const {
        const StringView text = line(index);
        if (tabWidth == 0) tabWidth = 1;
        uint32_t column = 0;
        size_t   i = 0;
        while (i < text.size() && i < byte) {
                if (text[i] == '\t') {
                        column = (column / tabWidth + 1) * tabWidth;
                        ++i;
                        continue;
                }
                column += characterColumns(Utf8::decode(text, i));
        }
        return column;
}

uint32_t TextDocument::byteAtColumn(uint32_t index, uint32_t column, uint32_t tabWidth) const {
        const StringView text = line(index);
        if (tabWidth == 0) tabWidth = 1;
        uint32_t at = 0;
        size_t   i = 0;
        while (i < text.size()) {
                const size_t start = i;
                uint32_t     width;
                if (text[i] == '\t') {
                        width = (at / tabWidth + 1) * tabWidth - at;
                        ++i;
                } else {
                        width = characterColumns(Utf8::decode(text, i));
                }
                // The nearer edge of the character the column falls in.
                if (column < at + width) return static_cast<uint32_t>(column - at < (width + 1) / 2 ? start : i);
                at += width;
        }
        return static_cast<uint32_t>(text.size());
}

// ---- Editing

TextPosition TextDocument::endOfInsert(TextPosition at, StringView text) const {
        const size_t nl = text.lastIndexOf('\n');
        if (nl == StringView::NPos) return TextPosition{at.line, at.byte + static_cast<uint32_t>(text.size())};
        uint32_t lines = 0;
        for (char c : text)
                if (c == '\n') ++lines;
        return TextPosition{at.line + lines, static_cast<uint32_t>(text.size() - nl - 1)};
}

Result<TextPosition> TextDocument::rawInsert(TextPosition at, StringView text) {
        at = clamp(at);
        if (text.isEmpty()) return Result<TextPosition>(at, Error::Ok);
        size_t newlines = 0;
        for (char c : text)
                if (c == '\n') ++newlines;
        if (newlines == 0) {
                Line &here = _lines[at.line];
                Error err = fitLine(here, here.length + text.size());
                if (err.isError()) return Result<TextPosition>(at, err);
                std::memmove(here.data + at.byte + text.size(), here.data + at.byte, here.length - at.byte);
                std::memcpy(here.data + at.byte, text.data(), text.size());
                here.length += static_cast<uint32_t>(text.size());
                here.state = 0;
                _length += text.size();
                notify(Change{at.line, 1, 1});
                return Result<TextPosition>(TextPosition{at.line, at.byte + static_cast<uint32_t>(text.size())},
                                            Error::Ok);
        }
        // The new lines made first, so running out of memory changes nothing: the last takes the
        // rest of this line and its ending; this line keeps its start and the first segment.
        if (newlines > UINT32_MAX - _count) return Result<TextPosition>(at, Error::NoMem);
        Error err = reserveLines(_count + static_cast<uint32_t>(newlines));
        if (err.isError()) return Result<TextPosition>(at, err);
        Line &here = _lines[at.line]; // the array may have moved
        auto *made = static_cast<Line *>(Memory::alloc(_space, sizeof(Line) * newlines));
        if (made == nullptr) return Result<TextPosition>(at, Error::NoMem);
        for (size_t n = 0; n < newlines; ++n) made[n] = Line{};
        const StringView tail(here.data + at.byte, here.length - at.byte);
        size_t           start = text.indexOf('\n') + 1;
        const size_t     firstEnd = start - 1;
        for (size_t n = 0; n < newlines && err.isOk(); ++n) {
                const bool   last = n + 1 == newlines;
                const size_t nl = last ? text.size() : text.indexOf('\n', start);
                Line        &line = made[n];
                const size_t length = nl - start + (last ? tail.size() : 0);
                err = fitLine(line, length);
                if (err.isError()) break;
                std::memcpy(line.data, text.data() + start, nl - start);
                if (last) std::memcpy(line.data + (nl - start), tail.data(), tail.size());
                line.length = static_cast<uint32_t>(length);
                line.crlf = last ? here.crlf : _ending == LineEnding::CrLf;
                start = nl + 1;
        }
        if (err.isOk()) err = fitLine(here, at.byte + firstEnd);
        if (err.isError()) {
                for (size_t n = 0; n < newlines; ++n) freeLine(made[n]);
                Memory::free(made);
                return Result<TextPosition>(at, err);
        }
        std::memcpy(here.data + at.byte, text.data(), firstEnd);
        here.length = at.byte + static_cast<uint32_t>(firstEnd);
        here.crlf = _ending == LineEnding::CrLf;
        here.state = 0;
        std::memmove(static_cast<void *>(_lines + at.line + 1 + newlines), _lines + at.line + 1,
                     sizeof(Line) * (_count - at.line - 1));
        std::memcpy(static_cast<void *>(_lines + at.line + 1), made, sizeof(Line) * newlines);
        Memory::free(made);
        _count += static_cast<uint32_t>(newlines);
        _length += text.size();
        notify(Change{at.line, 1, static_cast<uint32_t>(newlines + 1)});
        const Line &lastLine = _lines[at.line + newlines];
        return Result<TextPosition>(TextPosition{at.line + static_cast<uint32_t>(newlines),
                                                 lastLine.length - static_cast<uint32_t>(tail.size())},
                                    Error::Ok);
}

Error TextDocument::rawRemove(TextRange range) {
        range.start = clamp(range.start);
        range.end = clamp(range.end);
        if (range.end <= range.start) return Error::Ok;
        Line &first = _lines[range.start.line];
        if (range.start.line == range.end.line) {
                const uint32_t n = range.end.byte - range.start.byte;
                std::memmove(first.data + range.start.byte, first.data + range.end.byte, first.length - range.end.byte);
                first.length -= n;
                first.state = 0;
                _length -= n;
                notify(Change{range.start.line, 1, 1});
                return Error::Ok;
        }
        Line          &last = _lines[range.end.line];
        const uint32_t tail = last.length - range.end.byte;
        Error          err = fitLine(first, range.start.byte + tail);
        if (err.isError()) return err;
        // The bytes removed: the rest of the first line, the whole lines between, the last's start,
        // and a newline each.
        size_t removed = (first.length - range.start.byte) + range.end.byte;
        for (uint32_t i = range.start.line + 1; i < range.end.line; ++i) removed += _lines[i].length;
        removed += range.end.line - range.start.line;
        std::memcpy(first.data + range.start.byte, last.data + range.end.byte, tail);
        first.length = range.start.byte + tail;
        first.crlf = last.crlf;
        first.state = 0;
        for (uint32_t i = range.start.line + 1; i <= range.end.line; ++i) freeLine(_lines[i]);
        const uint32_t gone = range.end.line - range.start.line;
        std::memmove(static_cast<void *>(_lines + range.start.line + 1), _lines + range.end.line + 1,
                     sizeof(Line) * (_count - range.end.line - 1));
        _count -= gone;
        _length -= removed;
        notify(Change{range.start.line, gone + 1, 1});
        return Error::Ok;
}

Result<TextPosition> TextDocument::insert(TextPosition at, StringView text) {
        return replace(TextRange{at, at}, text);
}

Error TextDocument::remove(TextRange range) {
        return replace(range, StringView()).error();
}

Result<TextPosition> TextDocument::replace(TextRange range, StringView text) {
        range.start = clamp(range.start);
        range.end = clamp(range.end);
        if (range.end < range.start) range = TextRange{range.end, range.start};
        String             storage(MemSpace::Psram);
        Result<StringView> norm = normalized(text, storage);
        if (norm.isError()) return Result<TextPosition>(range.start, norm.error());
        text = norm.value();
        if (range.isEmpty() && text.isEmpty()) return Result<TextPosition>(range.start, Error::Ok);
        // An edit outside a step is a step of its own.
        const bool implicit = _depth == 0 && !_replaying;
        if (implicit) beginEdit(TextSelection{range.end, range.start});
        Error err = Error::Ok;
        if (!_replaying) err = record(range.start, range, text);
        TextPosition after = range.start;
        if (err.isOk()) err = rawRemove(range);
        if (err.isOk()) {
                Result<TextPosition> r = rawInsert(range.start, text);
                err = r.error();
                after = r.value();
        }
        if (err.isError() && !_replaying) clearUndo(); // the log no longer matches the text
        if (implicit) endEdit(TextSelection{after, after});
        return Result<TextPosition>(after, err);
}

Error TextDocument::record(TextPosition at, TextRange removed, StringView inserted) {
        if (_depth == 0 || _step == 0) return Error::Ok;
        Step &step = _steps[_step - 1];
        Edit  edit;
        edit.at = at;
        Error err = text(removed, edit.removed);
        if (err.isOk()) err = edit.inserted.assign(inserted);
        if (err.isOk()) {
                const size_t bytes = edit.removed.size() + edit.inserted.size();
                err = step.edits.append(std::move(edit));
                if (err.isOk()) {
                        step.bytes += bytes;
                        _undoBytes += bytes;
                }
        }
        return err;
}

// ---- Undo and redo

void TextDocument::dropRedo() {
        while (_steps.size() > _step) {
                _undoBytes -= _steps.back().bytes;
                _steps.removeAt(_steps.size() - 1);
        }
        if (_savedStep != NoStep && _savedStep > _step) _savedStep = NoStep;
        return;
}

void TextDocument::beginEdit(const TextSelection &before, EditKind kind, uint32_t nowMs, StringView typed) {
        if (_depth++ != 0 || _replaying) return;
        dropRedo();
        bool merge =
                kind != EditKind::Other && _mergeable && _step != 0 && _step == _steps.size() && _savedStep != _step;
        if (merge) {
                const Step &last = _steps[_step - 1];
                merge = last.kind == kind && nowMs - last.lastMs < MergeMs;
                // A word typed after something else starts a step of its own.
                if (merge && kind == EditKind::Typing && !typed.isEmpty() && isWordByte(typed[0]) &&
                    !isWordByte(_lastTyped))
                        merge = false;
        }
        if (merge) {
                _steps[_step - 1].lastMs = nowMs;
        } else {
                Step step;
                step.before = before;
                step.after = before;
                step.kind = kind;
                step.lastMs = nowMs;
                if (_steps.append(std::move(step)).isError()) {
                        clearUndo();
                        _depth = 1;
                        return;
                }
                ++_step;
        }
        _lastTyped = typed.isEmpty() ? 0 : typed.back();
        return;
}

void TextDocument::endEdit(const TextSelection &after) {
        if (_depth == 0) return;
        if (--_depth != 0 || _replaying) return;
        if (_step == 0) return;
        Step &step = _steps[_step - 1];
        if (step.edits.isEmpty()) {
                // Nothing changed: no step.
                _steps.removeAt(_step - 1);
                --_step;
                _mergeable = false;
                return;
        }
        step.after = after;
        _mergeable = step.kind != EditKind::Other;
        trimUndo();
        return;
}

void TextDocument::trimUndo() {
        // The oldest steps go, keeping the one just made.
        while (_undoBytes > _undoLimit && _step > 1) {
                _undoBytes -= _steps.front().bytes;
                _steps.removeAt(0);
                --_step;
                if (_savedStep != NoStep) _savedStep = _savedStep == 0 ? NoStep : _savedStep - 1;
        }
        return;
}

Result<TextSelection> TextDocument::undo() {
        if (_step == 0 || _depth != 0) return Result<TextSelection>(TextSelection{}, Error::Empty);
        const Step &step = _steps[_step - 1];
        _replaying = true;
        Error err = Error::Ok;
        for (size_t i = step.edits.size(); err.isOk() && i > 0; --i) {
                const Edit &e = step.edits[i - 1];
                err = replace(TextRange{e.at, endOfInsert(e.at, e.inserted.view())}, e.removed.view()).error();
        }
        _replaying = false;
        _mergeable = false;
        if (err.isError()) {
                clearUndo();
                return Result<TextSelection>(TextSelection{}, err);
        }
        --_step;
        return Result<TextSelection>(step.before, Error::Ok);
}

Result<TextSelection> TextDocument::redo() {
        if (_step >= _steps.size() || _depth != 0) return Result<TextSelection>(TextSelection{}, Error::Empty);
        const Step &step = _steps[_step];
        _replaying = true;
        Error err = Error::Ok;
        for (size_t i = 0; err.isOk() && i < step.edits.size(); ++i) {
                const Edit &e = step.edits[i];
                err = replace(TextRange{e.at, endOfInsert(e.at, e.removed.view())}, e.inserted.view()).error();
        }
        _replaying = false;
        _mergeable = false;
        if (err.isError()) {
                clearUndo();
                return Result<TextSelection>(TextSelection{}, err);
        }
        ++_step;
        return Result<TextSelection>(step.after, Error::Ok);
}

void TextDocument::clearUndo() {
        const bool modified = isModified();
        _steps.clear();
        _step = 0;
        _savedStep = modified ? NoStep : 0;
        _undoBytes = 0;
        _mergeable = false;
        return;
}

void TextDocument::setUndoLimit(size_t bytes) {
        _undoLimit = bytes;
        trimUndo();
        return;
}

// ---- Find and replace

bool TextDocument::matchesAt(TextPosition p, StringView needle, uint32_t flags) const {
        const Line &l = _lines[p.line];
        if (p.byte + needle.size() > l.length) return false;
        const char *at = l.data + p.byte;
        if ((flags & FindCaseSensitive) != 0) {
                if (std::memcmp(at, needle.data(), needle.size()) != 0) return false;
        } else {
                for (size_t i = 0; i < needle.size(); ++i)
                        if (lower(at[i]) != lower(needle[i])) return false;
        }
        if ((flags & FindWholeWord) != 0) {
                if (p.byte > 0 && isWordByte(l.data[p.byte - 1])) return false;
                const size_t after = p.byte + needle.size();
                if (after < l.length && isWordByte(l.data[after])) return false;
        }
        return true;
}

Result<TextRange> TextDocument::find(StringView needle, TextPosition from, uint32_t flags) const {
        if (needle.isEmpty() || needle.contains('\n')) return Result<TextRange>(TextRange{}, Error::Invalid);
        from = clamp(from);
        const auto n = static_cast<uint32_t>(needle.size());
        auto       found = [&](uint32_t line, uint32_t byte) {
                const TextPosition s{line, byte};
                return Result<TextRange>(TextRange{s, {line, byte + n}}, Error::Ok);
        };
        if ((flags & FindBackward) == 0) {
                for (int pass = 0; pass < 2; ++pass) {
                        const uint32_t firstLine = pass == 0 ? from.line : 0;
                        const uint32_t lastLine = pass == 0 ? _count - 1 : from.line;
                        for (uint32_t i = firstLine; i <= lastLine; ++i) {
                                const uint32_t start = (pass == 0 && i == from.line) ? from.byte : 0;
                                const uint32_t len = _lines[i].length;
                                for (uint32_t b = start; b + n <= len; ++b)
                                        if (matchesAt(TextPosition{i, b}, needle, flags)) {
                                                if (pass == 1 && i == from.line && b >= from.byte) break;
                                                return found(i, b);
                                        }
                        }
                        if ((flags & FindWrap) == 0) break;
                }
        } else {
                for (int pass = 0; pass < 2; ++pass) {
                        const uint32_t firstLine = pass == 0 ? from.line : _count - 1;
                        const uint32_t lastLine = pass == 0 ? 0 : from.line;
                        for (uint32_t i = firstLine + 1; i-- > lastLine;) {
                                const uint32_t len = _lines[i].length;
                                if (len < n) continue;
                                // Starting before `from` on its line (the first pass), anywhere elsewhere.
                                int64_t b = static_cast<int64_t>(len - n);
                                if (pass == 0 && i == from.line) b = static_cast<int64_t>(from.byte) - 1;
                                if (b > static_cast<int64_t>(len - n)) b = static_cast<int64_t>(len - n);
                                for (; b >= 0; --b) {
                                        if (pass == 1 && i == from.line && static_cast<uint32_t>(b) < from.byte) break;
                                        if (matchesAt(TextPosition{i, static_cast<uint32_t>(b)}, needle, flags))
                                                return found(i, static_cast<uint32_t>(b));
                                }
                        }
                        if ((flags & FindWrap) == 0) break;
                }
        }
        return Result<TextRange>(TextRange{}, Error::NotFound);
}

Result<size_t> TextDocument::replaceAll(StringView needle, StringView replacement, uint32_t flags) {
        if (needle.isEmpty() || needle.contains('\n')) return Result<size_t>(0, Error::Invalid);
        flags &= ~(FindBackward | FindWrap);
        const bool implicit = _depth == 0;
        if (implicit) beginEdit(TextSelection{});
        size_t       count = 0;
        TextPosition from{};
        Error        err = Error::Ok;
        TextPosition last{};
        for (;;) {
                Result<TextRange> r = find(needle, from, flags);
                if (r.isError()) break;
                Result<TextPosition> after = replace(r.value(), replacement);
                if (after.isError()) {
                        err = after.error();
                        break;
                }
                ++count;
                from = after.value();
                last = from;
        }
        if (implicit) endEdit(TextSelection{last, last});
        return Result<size_t>(count, err);
}

// ---- Observers

Error TextDocument::addObserver(Changed fn, void *arg) {
        for (Observer &o : _observers)
                if (o.fn == nullptr) {
                        o.fn = fn;
                        o.arg = arg;
                        return Error::Ok;
                }
        return Error::Full;
}

void TextDocument::removeObserver(Changed fn, void *arg) {
        for (Observer &o : _observers)
                if (o.fn == fn && o.arg == arg) o = Observer{};
        return;
}

void TextDocument::notify(const Change &change) {
        for (const Observer &o : _observers)
                if (o.fn != nullptr) o.fn(o.arg, change);
        return;
}

// ---- TextCursor

void TextCursor::setPosition(TextPosition p, bool select) {
        horizontal(_document->clamp(p), select);
        return;
}

void TextCursor::setSelection(const TextSelection &selection) {
        _selection.cursor = _document->clamp(selection.cursor);
        _selection.anchor = _document->clamp(selection.anchor);
        _wantColumn = -1;
        return;
}

void TextCursor::clamp() {
        _selection.cursor = _document->clamp(_selection.cursor);
        _selection.anchor = _document->clamp(_selection.anchor);
        return;
}

void TextCursor::horizontal(TextPosition to, bool select) {
        _selection.cursor = to;
        if (!select) _selection.anchor = to;
        _wantColumn = -1;
        return;
}

void TextCursor::vertical(uint32_t line, bool select) {
        if (_wantColumn < 0)
                _wantColumn = _document->columnOf(_selection.cursor.line, _selection.cursor.byte, _tabWidth);
        const uint32_t byte = _document->byteAtColumn(line, static_cast<uint32_t>(_wantColumn), _tabWidth);
        _selection.cursor = TextPosition{line, byte};
        if (!select) _selection.anchor = _selection.cursor;
        return;
}

void TextCursor::moveLeft(bool select) {
        // A selection collapses to its start.
        if (!select && hasSelection()) {
                horizontal(range().start, false);
                return;
        }
        horizontal(_document->previousCharacter(position()), select);
        return;
}

void TextCursor::moveRight(bool select) {
        if (!select && hasSelection()) {
                horizontal(range().end, false);
                return;
        }
        horizontal(_document->nextCharacter(position()), select);
        return;
}

void TextCursor::moveWordLeft(bool select) {
        horizontal(_document->previousWord(position()), select);
        return;
}

void TextCursor::moveWordRight(bool select) {
        horizontal(_document->nextWord(position()), select);
        return;
}

void TextCursor::moveUp(bool select, uint32_t lines) {
        const uint32_t line = position().line;
        if (line == 0) {
                const int64_t want = _wantColumn;
                horizontal(TextPosition{0, 0}, select);
                _wantColumn = want;
                return;
        }
        vertical(line > lines ? line - lines : 0, select);
        return;
}

void TextCursor::moveDown(bool select, uint32_t lines) {
        const uint32_t line = position().line;
        const uint32_t last = _document->lineCount() - 1;
        if (line == last) {
                const int64_t want = _wantColumn;
                horizontal(_document->end(), select);
                _wantColumn = want;
                return;
        }
        vertical(last - line > lines ? line + lines : last, select);
        return;
}

void TextCursor::moveLineStart(bool select, bool smart) {
        const TextPosition p = position();
        const StringView   text = _document->line(p.line);
        uint32_t           first = 0;
        while (first < text.size() && isSpace(text[first])) ++first;
        const uint32_t to = (!smart || p.byte == first) ? 0 : first;
        horizontal(TextPosition{p.line, to}, select);
        return;
}

void TextCursor::moveLineEnd(bool select) {
        const uint32_t line = position().line;
        horizontal(TextPosition{line, static_cast<uint32_t>(_document->line(line).size())}, select);
        return;
}

void TextCursor::moveDocumentStart(bool select) {
        horizontal(TextPosition{}, select);
        return;
}

void TextCursor::moveDocumentEnd(bool select) {
        horizontal(_document->end(), select);
        return;
}

void TextCursor::selectAll() {
        _selection.anchor = TextPosition{};
        _selection.cursor = _document->end();
        _wantColumn = -1;
        return;
}

void TextCursor::selectWord() {
        const TextRange word = _document->wordAt(position());
        _selection.anchor = word.start;
        _selection.cursor = word.end;
        _wantColumn = -1;
        return;
}

void TextCursor::selectLine() {
        const uint32_t line = position().line;
        _selection.anchor = TextPosition{line, 0};
        _selection.cursor = line + 1 < _document->lineCount()
                                    ? TextPosition{line + 1, 0}
                                    : TextPosition{line, static_cast<uint32_t>(_document->line(line).size())};
        _wantColumn = -1;
        return;
}

BMDS_NAMESPACE_END
