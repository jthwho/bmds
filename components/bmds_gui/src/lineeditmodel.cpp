/**
 * @file      lineeditmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A line of text being edited, and words wrapped in lines (devplan/gui.md §4q).
 */

#include <cstring>
#include <bmds/lineeditmodel.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isContinuation(char ch) {
                return Utf8::isContinuation(static_cast<uint8_t>(ch));
        }

} // namespace

size_t LineEditModel::before(size_t at) const {
        if (at == 0) return 0;
        --at;
        while (at > 0 && isContinuation(_text[at])) --at;
        return at;
}

size_t LineEditModel::after(size_t at) const {
        if (at >= _length) return _length;
        ++at;
        while (at < _length && isContinuation(_text[at])) ++at;
        return at;
}

void LineEditModel::setText(StringView text) {
        _length = 0;
        _cursor = _anchor = 0;
        (void)insert(text);
        _anchor = _cursor;
        return;
}

void LineEditModel::selectAll() {
        _anchor = 0;
        _cursor = _length;
        return;
}

void LineEditModel::removeSelection() {
        const size_t from = selectionStart(), to = selectionEnd();
        std::memmove(_text + from, _text + to, _length - to);
        _length -= to - from;
        _text[_length] = '\0';
        _cursor = _anchor = from;
        return;
}

bool LineEditModel::insert(StringView text) {
        bool changed = hasSelection();
        if (changed) removeSelection();
        size_t i = 0;
        while (i < text.size()) {
                // A whole character, or nothing.
                size_t n = 1;
                while (i + n < text.size() && isContinuation(text[i + n])) ++n;
                const auto lead = static_cast<uint8_t>(text[i]);
                if (lead < 0x20 || lead == 0x7f) { // controls (a pasted LF, a tab) left out
                        i += n;
                        continue;
                }
                if (_length + n > _max) break;
                std::memmove(_text + _cursor + n, _text + _cursor, _length - _cursor);
                std::memcpy(_text + _cursor, text.data() + i, n);
                _length += n;
                _cursor += n;
                i += n;
                changed = true;
        }
        _text[_length] = '\0';
        _anchor = _cursor;
        return changed;
}

bool LineEditModel::backspace() {
        if (hasSelection()) {
                removeSelection();
                return true;
        }
        if (_cursor == 0) return false;
        _anchor = before(_cursor);
        removeSelection();
        return true;
}

bool LineEditModel::erase() {
        if (hasSelection()) {
                removeSelection();
                return true;
        }
        if (_cursor == _length) return false;
        _anchor = after(_cursor);
        removeSelection();
        return true;
}

void LineEditModel::left(bool select) {
        if (!select && hasSelection()) {
                _cursor = _anchor = selectionStart();
                return;
        }
        _cursor = before(_cursor);
        if (!select) _anchor = _cursor;
        return;
}

void LineEditModel::right(bool select) {
        if (!select && hasSelection()) {
                _cursor = _anchor = selectionEnd();
                return;
        }
        _cursor = after(_cursor);
        if (!select) _anchor = _cursor;
        return;
}

void LineEditModel::setCursor(size_t offset, bool select) {
        if (offset > _length) offset = _length;
        while (offset > 0 && offset < _length && isContinuation(_text[offset])) --offset;
        _cursor = offset;
        if (!select) _anchor = _cursor;
        return;
}

size_t TextWrap::wrap(StringView text, uint16_t columns, Span<StringView> lines) {
        if (columns == 0) columns = 1;
        size_t count = 0;
        size_t at = 0;
        while (at < text.size() && count < lines.size()) {
                // Spaces at a line's start (after a break) go.
                while (at < text.size() && text[at] == ' ') ++at;
                if (at >= text.size()) break;
                size_t start = at, end = at, lastSpace = 0, cells = 0;
                bool   broke = false;
                while (end < text.size()) {
                        if (text[end] == '\n') {
                                broke = true;
                                break;
                        }
                        size_t         next = end;
                        const uint32_t cp = Utf8::decode(text, next);
                        const int      w = Utf8::displayWidth(cp);
                        const size_t   width = w < 0 ? 0 : size_t(w);
                        if (cells + width > columns) break;
                        if (text[end] == ' ') lastSpace = end;
                        cells += width;
                        end = next;
                }
                size_t cut = end;
                if (!broke && end < text.size() && text[end] != ' ' && lastSpace > start) cut = lastSpace;
                if (cut == start && !broke) cut = end > start ? end : start + 1; // a lone wide character too wide
                size_t trimmed = cut;
                while (trimmed > start && text[trimmed - 1] == ' ') --trimmed;
                lines[count++] = text.mid(start, trimmed - start);
                at = cut;
                if (at < text.size() && text[at] == '\n') ++at;
        }
        return count;
}

BMDS_NAMESPACE_END
