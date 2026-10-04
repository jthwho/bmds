/**
 * @file      utf8.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct Range {
                        uint32_t first;
                        uint32_t last;
        };

        // Zero-width: combining marks, format characters, variation selectors.
        constexpr Range ZeroWidth[] = {
                {0x0300, 0x036F},   {0x0483, 0x0489},   {0x0591, 0x05BD},   {0x05BF, 0x05BF},   {0x05C1, 0x05C2},
                {0x05C4, 0x05C5},   {0x05C7, 0x05C7},   {0x0600, 0x0605},   {0x0610, 0x061A},   {0x061C, 0x061C},
                {0x064B, 0x065F},   {0x0670, 0x0670},   {0x06D6, 0x06DD},   {0x06DF, 0x06E4},   {0x06E7, 0x06E8},
                {0x06EA, 0x06ED},   {0x070F, 0x070F},   {0x0711, 0x0711},   {0x0730, 0x074A},   {0x07A6, 0x07B0},
                {0x07EB, 0x07F3},   {0x0816, 0x082D},   {0x0859, 0x085B},   {0x08D3, 0x0902},   {0x093A, 0x093A},
                {0x093C, 0x093C},   {0x0941, 0x0948},   {0x094D, 0x094D},   {0x0951, 0x0957},   {0x0962, 0x0963},
                {0x0981, 0x0981},   {0x09BC, 0x09BC},   {0x09C1, 0x09C4},   {0x09CD, 0x09CD},   {0x09E2, 0x09E3},
                {0x0A01, 0x0A02},   {0x0A3C, 0x0A3C},   {0x0A41, 0x0A51},   {0x0A70, 0x0A71},   {0x0A75, 0x0A75},
                {0x0A81, 0x0A82},   {0x0ABC, 0x0ABC},   {0x0AC1, 0x0AC8},   {0x0ACD, 0x0ACD},   {0x0AE2, 0x0AE3},
                {0x0B01, 0x0B01},   {0x0B3C, 0x0B3C},   {0x0B3F, 0x0B3F},   {0x0B41, 0x0B44},   {0x0B4D, 0x0B4D},
                {0x0B56, 0x0B56},   {0x0B62, 0x0B63},   {0x0B82, 0x0B82},   {0x0BC0, 0x0BC0},   {0x0BCD, 0x0BCD},
                {0x0C00, 0x0C00},   {0x0C3E, 0x0C40},   {0x0C46, 0x0C56},   {0x0C62, 0x0C63},   {0x0C81, 0x0C81},
                {0x0CBC, 0x0CBC},   {0x0CCC, 0x0CCD},   {0x0CE2, 0x0CE3},   {0x0D00, 0x0D01},   {0x0D41, 0x0D44},
                {0x0D4D, 0x0D4D},   {0x0D62, 0x0D63},   {0x0DCA, 0x0DCA},   {0x0DD2, 0x0DD6},   {0x0E31, 0x0E31},
                {0x0E34, 0x0E3A},   {0x0E47, 0x0E4E},   {0x0EB1, 0x0EB1},   {0x0EB4, 0x0EBC},   {0x0EC8, 0x0ECD},
                {0x0F18, 0x0F19},   {0x0F35, 0x0F35},   {0x0F37, 0x0F37},   {0x0F39, 0x0F39},   {0x0F71, 0x0F7E},
                {0x0F80, 0x0F84},   {0x0F86, 0x0F87},   {0x0F8D, 0x0FBC},   {0x0FC6, 0x0FC6},   {0x102D, 0x1030},
                {0x1032, 0x1037},   {0x1039, 0x103A},   {0x103D, 0x103E},   {0x1058, 0x1059},   {0x105E, 0x1060},
                {0x1071, 0x1074},   {0x1082, 0x1082},   {0x1085, 0x1086},   {0x108D, 0x108D},   {0x109D, 0x109D},
                {0x1160, 0x11FF},   {0x135D, 0x135F},   {0x1712, 0x1714},   {0x1732, 0x1734},   {0x1752, 0x1753},
                {0x1772, 0x1773},   {0x17B4, 0x17B5},   {0x17B7, 0x17BD},   {0x17C6, 0x17C6},   {0x17C9, 0x17D3},
                {0x17DD, 0x17DD},   {0x180B, 0x180E},   {0x18A9, 0x18A9},   {0x1920, 0x1922},   {0x1927, 0x1928},
                {0x1932, 0x1932},   {0x1939, 0x193B},   {0x1A17, 0x1A18},   {0x1A1B, 0x1A1B},   {0x1A56, 0x1A7F},
                {0x1AB0, 0x1AFF},   {0x1B00, 0x1B03},   {0x1B34, 0x1B44},   {0x1B6B, 0x1B73},   {0x1B80, 0x1B81},
                {0x1BA2, 0x1BAD},   {0x1BE6, 0x1BF3},   {0x1C2C, 0x1C37},   {0x1CD0, 0x1CF9},   {0x1DC0, 0x1DFF},
                {0x200B, 0x200F},   {0x202A, 0x202E},   {0x2060, 0x2064},   {0x2066, 0x206F},   {0x20D0, 0x20F0},
                {0x2CEF, 0x2CF1},   {0x2D7F, 0x2D7F},   {0x2DE0, 0x2DFF},   {0x302A, 0x302D},   {0x3099, 0x309A},
                {0xA66F, 0xA672},   {0xA674, 0xA67D},   {0xA69E, 0xA69F},   {0xA6F0, 0xA6F1},   {0xA802, 0xA802},
                {0xA806, 0xA806},   {0xA80B, 0xA80B},   {0xA825, 0xA826},   {0xA8C4, 0xA8C5},   {0xA8E0, 0xA8F1},
                {0xA926, 0xA92D},   {0xA947, 0xA951},   {0xA980, 0xA982},   {0xA9B3, 0xA9B3},   {0xA9B6, 0xA9B9},
                {0xA9BC, 0xA9BD},   {0xAA29, 0xAA2E},   {0xAA31, 0xAA32},   {0xAA35, 0xAA36},   {0xAA43, 0xAA43},
                {0xAA4C, 0xAA4C},   {0xAAB0, 0xAAB0},   {0xAAB2, 0xAAB4},   {0xAAB7, 0xAAB8},   {0xAABE, 0xAABF},
                {0xAAC1, 0xAAC1},   {0xABE5, 0xABE5},   {0xABE8, 0xABE8},   {0xABED, 0xABED},   {0xFB1E, 0xFB1E},
                {0xFE00, 0xFE0F},   {0xFE20, 0xFE2F},   {0xFEFF, 0xFEFF},   {0xFFF9, 0xFFFB},   {0x101FD, 0x101FD},
                {0x10A01, 0x10A0F}, {0x10A38, 0x10A3F}, {0x11000, 0x11002}, {0x11038, 0x11046}, {0x1D167, 0x1D169},
                {0x1D173, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244}, {0x1F3FB, 0x1F3FF},
                {0xE0001, 0xE007F}, {0xE0100, 0xE01EF},
        };

        // Two columns: East Asian wide and fullwidth, and emoji presentation.
        constexpr Range Wide[] = {
                {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},
                {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
                {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
                {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
                {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},
                {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
                {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
                {0x2E80, 0x303E},   {0x3041, 0x3247},   {0x3250, 0x4DBF},   {0x4E00, 0xA4CF},   {0xA960, 0xA97F},
                {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},
                {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4}, {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004},
                {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F},
                {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD},
                {0x30000, 0x3FFFD},
        };

        template <size_t N> bool inTable(const Range (&table)[N], uint32_t c) {
                size_t lo = 0, hi = N;
                while (lo < hi) {
                        const size_t mid = lo + (hi - lo) / 2;
                        if (c > table[mid].last) {
                                lo = mid + 1;
                        } else if (c < table[mid].first) {
                                hi = mid;
                        } else {
                                return true;
                        }
                }
                return false;
        }

        // Sequence length and initial value bits for a lead byte; 0 if invalid.
        uint8_t leadLength(uint8_t b, uint32_t &bits, uint32_t &min) {
                if (b < 0x80) {
                        bits = b;
                        min = 0;
                        return 1;
                }
                if (b >= 0xC2 && b <= 0xDF) {
                        bits = b & 0x1F;
                        min = 0x80;
                        return 2;
                }
                if (b >= 0xE0 && b <= 0xEF) {
                        bits = b & 0x0F;
                        min = 0x800;
                        return 3;
                }
                if (b >= 0xF0 && b <= 0xF4) {
                        bits = b & 0x07;
                        min = 0x10000;
                        return 4;
                }
                return 0;
        }

        bool valid(uint32_t c, uint32_t min) {
                return c >= min && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
        }

} // namespace

uint32_t Utf8::decode(StringView text, size_t &i) {
        uint32_t     c = 0, min = 0;
        const size_t len = leadLength(static_cast<uint8_t>(text[i]), c, min);
        if (len == 0 || i + len > text.size()) {
                ++i;
                return Replacement;
        }
        for (size_t k = 1; k < len; ++k) {
                const uint8_t b = static_cast<uint8_t>(text[i + k]);
                if (!isContinuation(b)) {
                        ++i;
                        return Replacement;
                }
                c = (c << 6) | (b & 0x3F);
        }
        if (!valid(c, min)) {
                ++i;
                return Replacement;
        }
        i += len;
        return c;
}

size_t Utf8::encode(uint32_t c, char out[4]) {
        if (c >= 0xD800 && c <= 0xDFFF) c = Replacement;
        if (c < 0x80) {
                out[0] = static_cast<char>(c);
                return 1;
        }
        if (c < 0x800) {
                out[0] = static_cast<char>(0xC0 | (c >> 6));
                out[1] = static_cast<char>(0x80 | (c & 0x3F));
                return 2;
        }
        if (c > 0x10FFFF) c = Replacement;
        if (c < 0x10000) {
                out[0] = static_cast<char>(0xE0 | (c >> 12));
                out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out[2] = static_cast<char>(0x80 | (c & 0x3F));
                return 3;
        }
        out[0] = static_cast<char>(0xF0 | (c >> 18));
        out[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (c & 0x3F));
        return 4;
}

int Utf8::displayWidth(uint32_t c) {
        if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
        if (c < 0x300) return 1;
        if (inTable(ZeroWidth, c)) return 0;
        if (inTable(Wide, c)) return 2;
        return 1;
}

size_t Utf8::displayWidth(StringView text) {
        size_t width = 0;
        for (size_t i = 0; i < text.size();) width += static_cast<size_t>(displayWidth(decode(text, i)));
        return width;
}

size_t Utf8Decoder::feed(uint8_t b, uint32_t out[2]) {
        size_t n = 0;
        if (_need > 0) {
                if (Utf8::isContinuation(b)) {
                        _codepoint = (_codepoint << 6) | (b & 0x3F);
                        if (--_need > 0) return 0;
                        out[0] = valid(_codepoint, _min) ? _codepoint : Utf8::Replacement;
                        return 1;
                }
                // Cut short: the sequence so far is one bad character; then this byte.
                _need = 0;
                out[n++] = Utf8::Replacement;
        }
        uint32_t     bits = 0, min = 0;
        const size_t len = leadLength(b, bits, min);
        if (len == 1) {
                out[n++] = bits;
        } else if (len == 0) {
                out[n++] = Utf8::Replacement;
        } else {
                _codepoint = bits;
                _min = min;
                _need = static_cast<uint8_t>(len - 1);
        }
        return n;
}

BMDS_NAMESPACE_END
