/**
 * @file      jsonwriter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/jsonwriter.h>

BMDS_NAMESPACE_BEGIN

Error JsonWriter::appendTo(void *arg, StringView bytes) {
        return static_cast<String *>(arg)->append(bytes);
}

void JsonWriter::fail(Error err) {
        if (_error.isOk()) _error = err;
        return;
}

void JsonWriter::flush() {
        if (_used == 0) return;
        if (_error.isOk()) fail(_sink(_arg, StringView(_buffer, _used)));
        _used = 0;
        return;
}

void JsonWriter::put(char ch) {
        if (_used == sizeof(_buffer)) flush();
        _buffer[_used++] = ch;
        return;
}

void JsonWriter::put(StringView bytes) {
        const char *p = bytes.data();
        size_t      n = bytes.size();
        while (n > 0) {
                if (_used == sizeof(_buffer)) flush();
                const size_t room = sizeof(_buffer) - _used;
                const size_t take = n < room ? n : room;
                std::memcpy(_buffer + _used, p, take);
                _used = static_cast<uint8_t>(_used + take);
                p += take;
                n -= take;
        }
        return;
}

void JsonWriter::newline() {
        if (_indent == 0) return;
        put('\n');
        for (size_t i = 0; i < size_t(_depth) * _indent; ++i) put(' ');
        return;
}

bool JsonWriter::beginValue() {
        if (_error.isError()) return false;
        if (_afterKey) {
                _afterKey = false;
                return true;
        }
        if (_depth == 0) {
                if (_done) { // one value at the top
                        fail(Error::InvalidState);
                        return false;
                }
                return true;
        }
        if (inObject()) { // a value where a key belongs
                fail(Error::InvalidState);
                return false;
        }
        if (!_first) put(',');
        _first = false;
        newline();
        return true;
}

void JsonWriter::open(char ch, bool isObject) {
        if (!beginValue()) return;
        if (_depth == MaxDepth) {
                fail(Error::OutOfRange);
                return;
        }
        if (isObject) {
                _objects |= uint64_t(1) << _depth;
        } else {
                _objects &= ~(uint64_t(1) << _depth);
        }
        ++_depth;
        _first = true;
        put(ch);
        return;
}

void JsonWriter::close(char ch, bool isObject) {
        if (_error.isError()) return;
        if (_depth == 0 || inObject() != isObject || _afterKey) {
                fail(Error::InvalidState);
                return;
        }
        const bool empty = _first;
        --_depth;
        if (!empty) newline();
        put(ch);
        _first = false;
        if (_depth == 0) _done = true;
        return;
}

void JsonWriter::key(StringView name) {
        if (_error.isError()) return;
        if (!inObject() || _afterKey) {
                fail(Error::InvalidState);
                return;
        }
        if (!_first) put(',');
        _first = false;
        newline();
        writeString(name);
        put(':');
        if (_indent != 0) put(' ');
        _afterKey = true;
        return;
}

void JsonWriter::scalar(StringView text) {
        if (!beginValue()) return;
        put(text);
        if (_depth == 0) _done = true;
        return;
}

void JsonWriter::value(StringView text) {
        if (!beginValue()) return;
        writeString(text);
        if (_depth == 0) _done = true;
        return;
}

void JsonWriter::writeUnsigned(uint64_t number) {
        char  digits[20];
        char *p = digits + sizeof(digits);
        do {
                *--p = static_cast<char>('0' + number % 10);
                number /= 10;
        } while (number != 0);
        scalar(StringView(p, size_t(digits + sizeof(digits) - p)));
        return;
}

void JsonWriter::writeSigned(int64_t number) {
        if (number >= 0) {
                writeUnsigned(static_cast<uint64_t>(number));
                return;
        }
        // The magnitude as unsigned: INT64_MIN has no positive int64_t.
        uint64_t magnitude = ~static_cast<uint64_t>(number) + 1;
        char     digits[21];
        char    *p = digits + sizeof(digits);
        do {
                *--p = static_cast<char>('0' + magnitude % 10);
                magnitude /= 10;
        } while (magnitude != 0);
        *--p = '-';
        scalar(StringView(p, size_t(digits + sizeof(digits) - p)));
        return;
}

void JsonWriter::value(double number) {
        if (!std::isfinite(number)) {
                null();
                return;
        }
        // The shortest of 15 and 17 digits that reads back the same: 21.3, not 21.300000000000001.
        char text[32];
        int  n = std::snprintf(text, sizeof(text), "%.15g", number);
        if (n > 0 && std::strtod(text, nullptr) != number) n = std::snprintf(text, sizeof(text), "%.17g", number);
        if (n <= 0 || size_t(n) >= sizeof(text)) {
                fail(Error::Invalid);
                return;
        }
        scalar(StringView(text, size_t(n)));
        return;
}

void JsonWriter::writeString(StringView text) {
        static constexpr char Hex[] = "0123456789abcdef";
        put('"');
        size_t run = 0; // bytes from `start` that need no escape
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                const char         *escape = nullptr;
                switch (c) {
                        case '"': escape = "\\\""; break;
                        case '\\': escape = "\\\\"; break;
                        case '\b': escape = "\\b"; break;
                        case '\f': escape = "\\f"; break;
                        case '\n': escape = "\\n"; break;
                        case '\r': escape = "\\r"; break;
                        case '\t': escape = "\\t"; break;
                        default: break;
                }
                if (escape == nullptr && c >= 0x20) {
                        ++run;
                        continue;
                }
                put(text.mid(start, run));
                if (escape != nullptr) {
                        put(StringView(escape));
                } else {
                        const char u[6] = {'\\', 'u', '0', '0', Hex[c >> 4], Hex[c & 15]};
                        put(StringView(u, sizeof(u)));
                }
                start = i + 1;
                run = 0;
        }
        put(text.mid(start, run));
        put('"');
        return;
}

Error JsonWriter::finish() {
        flush();
        if (_error.isOk() && (_depth != 0 || _afterKey || !_done)) fail(Error::Invalid);
        return _error;
}

BMDS_NAMESPACE_END
