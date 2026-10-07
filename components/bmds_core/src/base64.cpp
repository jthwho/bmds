/**
 * @file      base64.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Base64 in pieces (devplan/clipboard.md C11).
 */

#include <bmds/base64.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        // A character's sextet, or -1.
        constexpr int sextet(char ch) {
                if (ch >= 'A' && ch <= 'Z') return ch - 'A';
                if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
                if (ch >= '0' && ch <= '9') return ch - '0' + 52;
                if (ch == '+') return 62;
                if (ch == '/') return 63;
                return -1;
        }

} // namespace

size_t Base64Decoder::put(StringView text, uint8_t *out) {
        size_t n = 0;
        for (const char ch : text) {
                if (_failed) return n;
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                if (ch == '=') {
                        // Padding ends a quantum of two or three sextets.
                        if (_count < 2 || _ended) {
                                _failed = true;
                                return n;
                        }
                        ++_pad;
                } else {
                        const int v = sextet(ch);
                        if (v < 0 || _pad != 0 || _ended) {
                                _failed = true;
                                return n;
                        }
                        _bits = (_bits << 6) | static_cast<uint32_t>(v);
                        ++_count;
                }
                if (_count + _pad < 4) continue;
                const uint32_t bits = _bits << (6 * _pad);
                out[n++] = static_cast<uint8_t>(bits >> 16);
                if (_count > 2) out[n++] = static_cast<uint8_t>(bits >> 8);
                if (_count > 3) out[n++] = static_cast<uint8_t>(bits);
                _ended = _pad != 0; // padding ends the text
                _bits = 0;
                _count = _pad = 0;
        }
        return n;
}

Result<size_t> Base64Decoder::finish(uint8_t *out) {
        const bool bad = _failed || _pad != 0 || _count == 1;
        size_t     n = 0;
        if (!bad && _count > 1) {
                const uint32_t bits = _bits << (6 * (4 - _count));
                out[n++] = static_cast<uint8_t>(bits >> 16);
                if (_count > 2) out[n++] = static_cast<uint8_t>(bits >> 8);
        }
        reset();
        if (bad) return makeError<size_t>(Error::CorruptData);
        return makeResult(n);
}

size_t Base64Encoder::put(ByteSpan data, char *out) {
        size_t n = 0;
        for (const uint8_t b : data) {
                if (_count < 2) {
                        _held[_count++] = b;
                        continue;
                }
                const uint32_t bits = (uint32_t(_held[0]) << 16) | (uint32_t(_held[1]) << 8) | b;
                out[n++] = Alphabet[(bits >> 18) & 63];
                out[n++] = Alphabet[(bits >> 12) & 63];
                out[n++] = Alphabet[(bits >> 6) & 63];
                out[n++] = Alphabet[bits & 63];
                _count = 0;
        }
        return n;
}

size_t Base64Encoder::finish(char *out) {
        if (_count == 0) return 0;
        const uint32_t bits = (uint32_t(_held[0]) << 16) | (_count > 1 ? uint32_t(_held[1]) << 8 : 0);
        out[0] = Alphabet[(bits >> 18) & 63];
        out[1] = Alphabet[(bits >> 12) & 63];
        out[2] = _count > 1 ? Alphabet[(bits >> 6) & 63] : '=';
        out[3] = '=';
        _count = 0;
        return 4;
}

BMDS_NAMESPACE_END
