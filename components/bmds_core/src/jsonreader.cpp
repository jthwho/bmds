/**
 * @file      jsonreader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdlib>
#include <bmds/jsonreader.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int hexValue(int c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        bool isDigit(int c) {
                return c >= '0' && c <= '9';
        }

} // namespace

JsonReader::JsonReader(StringView text, char *scratch, size_t scratchBytes)
    : _data(text.data()), _buffer(nullptr), _bufferBytes(0), _have(text.size()), _scratch(scratch),
      _scratchBytes(scratchBytes), _ended(true) {
        if (_scratch != nullptr && _scratchBytes > 0) _scratch[0] = '\0';
}

JsonReader::JsonReader(Source source, void *arg, char *buffer, size_t bufferBytes, char *scratch, size_t scratchBytes)
    : _source(source), _arg(arg), _data(buffer), _buffer(buffer), _bufferBytes(bufferBytes), _have(0),
      _scratch(scratch), _scratchBytes(scratchBytes) {
        if (_scratch != nullptr && _scratchBytes > 0) _scratch[0] = '\0';
}

int JsonReader::peek() {
        if (_pos < _have) return static_cast<unsigned char>(_data[_pos]);
        if (_error.isError()) return -2;
        if (_ended || _buffer == nullptr || _bufferBytes == 0) return -1;
        const Result<size_t> got = _source(_arg, _buffer, _bufferBytes);
        if (got.isError()) {
                _error = got.error();
                return -2;
        }
        if (got.value() == 0) {
                _ended = true;
                return -1;
        }
        _have = got.value() < _bufferBytes ? got.value() : _bufferBytes;
        _pos = 0;
        return static_cast<unsigned char>(_data[0]);
}

void JsonReader::advance() {
        if (_data[_pos] == '\n') {
                ++_line;
                _column = 1;
        } else {
                ++_column;
        }
        ++_pos;
        return;
}

void JsonReader::skipWhitespace() {
        for (;;) {
                const int c = peek();
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
                advance();
        }
}

JsonReader::Token JsonReader::fail(bmds::Error err) {
        if (_error.isOk()) _error = err; // a source's error stands
        _state = State::Failed;
        return Token::Error;
}

bool JsonReader::store(char ch) {
        if (_length + 1 >= _scratchBytes) return false; // the NUL's byte kept
        _scratch[_length++] = ch;
        _scratch[_length] = '\0';
        return true;
}

bool JsonReader::push(bool isObject) {
        if (_depth == MaxDepth) return false;
        if (isObject) {
                _objects |= uint64_t(1) << _depth;
        } else {
                _objects &= ~(uint64_t(1) << _depth);
        }
        ++_depth;
        return true;
}

JsonReader::Token JsonReader::close(bool isObject) {
        if (_depth == 0 || inObject() != isObject) return fail(bmds::Error::CorruptData);
        advance();
        --_depth;
        _state = State::AfterValue;
        return isObject ? Token::EndObject : Token::EndArray;
}

JsonReader::Token JsonReader::next() {
        if (_state == State::Failed) return Token::Error;
        if (_state == State::Done) return Token::End;
        _length = 0;
        _isNumber = false;
        if (_scratchBytes > 0) _scratch[0] = '\0';
        for (;;) {
                skipWhitespace();
                const int c = peek();
                if (c == -2) return fail(_error);
                switch (_state) {
                        case State::AfterValue:
                                if (_depth == 0) {
                                        if (c != -1) return fail(bmds::Error::CorruptData);
                                        _state = State::Done;
                                        return Token::End;
                                }
                                if (c == ',') {
                                        advance();
                                        _state = inObject() ? State::Key : State::Value;
                                        continue;
                                }
                                if (c == ']') return close(false);
                                if (c == '}') return close(true);
                                return fail(bmds::Error::CorruptData);
                        case State::KeyOrEnd:
                                if (c == '}') return close(true);
                                [[fallthrough]];
                        case State::Key:
                                if (c != '"') return fail(bmds::Error::CorruptData);
                                return string(Token::Key);
                        case State::AfterKey:
                                if (c != ':') return fail(bmds::Error::CorruptData);
                                advance();
                                _state = State::Value;
                                continue;
                        case State::ValueOrEnd:
                                if (c == ']') return close(false);
                                [[fallthrough]];
                        case State::Value: return value();
                        case State::Done: return Token::End;
                        case State::Failed: return Token::Error;
                }
        }
}

JsonReader::Token JsonReader::value() {
        const int c = peek();
        switch (c) {
                case '{':
                        if (!push(true)) return fail(bmds::Error::OutOfRange);
                        advance();
                        _state = State::KeyOrEnd;
                        return Token::BeginObject;
                case '[':
                        if (!push(false)) return fail(bmds::Error::OutOfRange);
                        advance();
                        _state = State::ValueOrEnd;
                        return Token::BeginArray;
                case '"': return string(Token::String);
                case 't': return literal("true", Token::True);
                case 'f': return literal("false", Token::False);
                case 'n': return literal("null", Token::Null);
                default:
                        if (c == '-' || isDigit(c)) return number();
                        return fail(bmds::Error::CorruptData);
        }
}

JsonReader::Token JsonReader::literal(const char *word, Token kind) {
        for (const char *p = word; *p != '\0'; ++p) {
                const int c = peek();
                if (c == -2) return fail(_error);
                if (c != *p) return fail(bmds::Error::CorruptData);
                advance();
        }
        _state = State::AfterValue;
        return kind;
}

JsonReader::Token JsonReader::number() {
        // -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?, kept as written.
        auto take = [this]() -> bool {
                if (!store(static_cast<char>(peek()))) return false;
                advance();
                return true;
        };
        auto digits = [this, &take]() -> int { // 1: some; 0: none; -1: no room
                int n = 0;
                while (isDigit(peek())) {
                        if (!take()) return -1;
                        n = 1;
                }
                return n;
        };
        _integer = true;
        if (peek() == '-' && !take()) return fail(bmds::Error::BufferTooSmall);
        if (peek() == '0') {
                if (!take()) return fail(bmds::Error::BufferTooSmall);
        } else {
                const int n = digits();
                if (n < 0) return fail(bmds::Error::BufferTooSmall);
                if (n == 0) return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
        }
        if (peek() == '.') {
                _integer = false;
                if (!take()) return fail(bmds::Error::BufferTooSmall);
                const int n = digits();
                if (n < 0) return fail(bmds::Error::BufferTooSmall);
                if (n == 0) return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
        }
        if (peek() == 'e' || peek() == 'E') {
                _integer = false;
                if (!take()) return fail(bmds::Error::BufferTooSmall);
                if ((peek() == '+' || peek() == '-') && !take()) return fail(bmds::Error::BufferTooSmall);
                const int n = digits();
                if (n < 0) return fail(bmds::Error::BufferTooSmall);
                if (n == 0) return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
        }
        if (peek() == -2) return fail(_error);
        _state = State::AfterValue;
        _isNumber = true;
        return Token::Number;
}

JsonReader::Token JsonReader::string(Token kind) {
        advance(); // the opening quote
        for (;;) {
                int c = peek();
                if (c == -2) return fail(_error);
                if (c == -1 || c < 0x20) return fail(bmds::Error::CorruptData);
                if (c == '"') {
                        advance();
                        break;
                }
                if (c == '\\') {
                        advance();
                        c = peek();
                        if (c == -2) return fail(_error);
                        char plain = 0;
                        switch (c) {
                                case '"': plain = '"'; break;
                                case '\\': plain = '\\'; break;
                                case '/': plain = '/'; break;
                                case 'b': plain = '\b'; break;
                                case 'f': plain = '\f'; break;
                                case 'n': plain = '\n'; break;
                                case 'r': plain = '\r'; break;
                                case 't': plain = '\t'; break;
                                case 'u': break;
                                default: return fail(bmds::Error::CorruptData);
                        }
                        advance();
                        if (c != 'u') {
                                if (!store(plain)) return fail(bmds::Error::BufferTooSmall);
                                continue;
                        }
                        // \uXXXX, a surrogate pair as two of them.
                        auto hex4 = [this]() -> int32_t {
                                int32_t v = 0;
                                for (int i = 0; i < 4; ++i) {
                                        const int h = hexValue(peek());
                                        if (h < 0) return -1;
                                        v = v << 4 | h;
                                        advance();
                                }
                                return v;
                        };
                        int32_t cp = hex4();
                        if (cp < 0) return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
                        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail(bmds::Error::CorruptData); // a lone low half
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (peek() != '\\')
                                        return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
                                advance();
                                if (peek() != 'u')
                                        return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
                                advance();
                                const int32_t low = hex4();
                                if (low < 0xDC00 || low > 0xDFFF)
                                        return fail(_error.isError() ? _error : bmds::Error(bmds::Error::CorruptData));
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        char         out[4];
                        const size_t n = Utf8::encode(static_cast<uint32_t>(cp), out);
                        for (size_t i = 0; i < n; ++i) {
                                if (!store(out[i])) return fail(bmds::Error::BufferTooSmall);
                        }
                        continue;
                }
                if (c < 0x80) {
                        if (!store(static_cast<char>(c))) return fail(bmds::Error::BufferTooSmall);
                        advance();
                        continue;
                }
                // UTF-8, checked: the lead byte, then its continuations' ranges (no overlong forms,
                // no surrogates, nothing past U+10FFFF).
                int     more = 0;
                uint8_t low = 0x80;
                uint8_t high = 0xBF;
                if (c >= 0xC2 && c <= 0xDF) {
                        more = 1;
                } else if (c >= 0xE0 && c <= 0xEF) {
                        more = 2;
                        if (c == 0xE0) low = 0xA0;
                        if (c == 0xED) high = 0x9F;
                } else if (c >= 0xF0 && c <= 0xF4) {
                        more = 3;
                        if (c == 0xF0) low = 0x90;
                        if (c == 0xF4) high = 0x8F;
                } else {
                        return fail(bmds::Error::CorruptData);
                }
                if (!store(static_cast<char>(c))) return fail(bmds::Error::BufferTooSmall);
                advance();
                for (int i = 0; i < more; ++i) {
                        const int b = peek();
                        if (b == -2) return fail(_error);
                        if (b < low || b > high) return fail(bmds::Error::CorruptData);
                        if (!store(static_cast<char>(b))) return fail(bmds::Error::BufferTooSmall);
                        advance();
                        low = 0x80;
                        high = 0xBF;
                }
        }
        _state = kind == Token::Key ? State::AfterKey : State::AfterValue;
        return kind;
}

Error JsonReader::skip() {
        if (_state == State::Failed) return _error;
        // Just after a Begin the state says so; the depth it opened is the current one.
        if (_state != State::KeyOrEnd && _state != State::ValueOrEnd) return Error::Ok;
        const size_t depth = _depth;
        while (_depth >= depth) {
                const Token t = next();
                if (t == Token::Error) return _error;
                if (t == Token::End) return Error::CorruptData;
        }
        return Error::Ok;
}

Error JsonReader::toUint64(uint64_t &out) const {
        const StringView t = text();
        if (!_isNumber) return Error::Invalid;
        if (t[0] == '-') {
                // -0 is 0; anything else below zero doesn't fit.
                for (size_t i = 1; i < t.size(); ++i) {
                        if (t[i] != '0') return _integer ? Error::OutOfRange : Error::Invalid;
                }
                if (!_integer) return Error::Invalid;
                out = 0;
                return Error::Ok;
        }
        if (!_integer || !isDigit(t[0])) return Error::Invalid;
        uint64_t v = 0;
        for (size_t i = 0; i < t.size(); ++i) {
                const unsigned d = static_cast<unsigned>(t[i] - '0');
                if (d > 9) return Error::Invalid;
                if (v > (UINT64_MAX - d) / 10) return Error::OutOfRange;
                v = v * 10 + d;
        }
        out = v;
        return Error::Ok;
}

Error JsonReader::toInt64(int64_t &out) const {
        const StringView t = text();
        if (!_isNumber || !_integer) return Error::Invalid;
        const bool negative = t[0] == '-';
        uint64_t   v = 0;
        for (size_t i = negative ? 1 : 0; i < t.size(); ++i) {
                const unsigned d = static_cast<unsigned>(t[i] - '0');
                if (d > 9) return Error::Invalid;
                if (v > (UINT64_MAX - d) / 10) return Error::OutOfRange;
                v = v * 10 + d;
        }
        const uint64_t limit = negative ? uint64_t(INT64_MAX) + 1 : uint64_t(INT64_MAX);
        if (v > limit) return Error::OutOfRange;
        out = negative ? static_cast<int64_t>(~v + 1) : static_cast<int64_t>(v);
        return Error::Ok;
}

Error JsonReader::toDouble(double &out) const {
        if (!_isNumber) return Error::Invalid;
        char        *end = nullptr;
        const double v = std::strtod(_scratch, &end); // the scratch is NUL-ended
        if (end != _scratch + _length) return Error::Invalid;
        out = v;
        return Error::Ok;
}

Error JsonReader::toFloat(float &out) const {
        if (!_isNumber) return Error::Invalid;
        char       *end = nullptr;
        const float v = std::strtof(_scratch, &end);
        if (end != _scratch + _length) return Error::Invalid;
        out = v;
        return Error::Ok;
}

BMDS_NAMESPACE_END
