/**
 * @file      fontname.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <bmds/fontname.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *RoleWords[] = {"", "ui", "mono", "title", "terminal"};

        bool sameWord(StringView a, const char *b) {
                const size_t n = std::strlen(b);
                if (a.size() != n) return false;
                for (size_t i = 0; i < n; ++i) {
                        char c = a[i];
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        if (c != b[i]) return false;
                }
                return true;
        }

        bool isNumber(StringView word) {
                if (word.isEmpty() || word.size() > 5) return false;
                for (char c : word)
                        if (c < '0' || c > '9') return false;
                return true;
        }

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

} // namespace

Result<FontName> FontName::parse(StringView text) {
        // The words, at most 16, as views into the text.
        StringView words[16];
        size_t     count = 0;
        for (size_t i = 0; i < text.size();) {
                while (i < text.size() && isSpace(text[i])) ++i;
                const size_t start = i;
                while (i < text.size() && !isSpace(text[i])) ++i;
                if (i == start) break;
                if (count == sizeof(words) / sizeof(words[0])) return makeError<FontName>(Error::OutOfRange);
                words[count++] = text.mid(start, i - start);
        }
        FontName name;
        // From the end: the size and style words.
        while (count > 0) {
                const StringView w = words[count - 1];
                if (isNumber(w) && name.size == 0) {
                        unsigned v = 0;
                        for (char c : w) v = v * 10 + unsigned(c - '0');
                        if (v < MinSize || v > MaxSize) return makeError<FontName>(Error::OutOfRange);
                        name.size = static_cast<uint16_t>(v);
                } else if (sameWord(w, "bold")) {
                        name.style |= Bold;
                        name.hasStyle = true;
                } else if (sameWord(w, "italic")) {
                        name.style |= Italic;
                        name.hasStyle = true;
                } else if (sameWord(w, "regular")) {
                        name.hasStyle = true;
                } else {
                        break;
                }
                --count;
        }
        if (count == 0) return makeError<FontName>(Error::Invalid);
        if (count == 1) {
                name.role = roleFromName(words[0]);
                if (name.role != Role::None) return makeResult(name);
        }
        for (size_t i = 0; i < count; ++i) {
                const size_t need = words[i].size() + (i > 0 ? 1 : 0);
                if (name.familyLength + need > MaxFamily) return makeError<FontName>(Error::OutOfRange);
                if (i > 0) name.family[name.familyLength++] = ' ';
                std::memcpy(name.family + name.familyLength, words[i].data(), words[i].size());
                name.familyLength = static_cast<uint8_t>(name.familyLength + words[i].size());
        }
        name.family[name.familyLength] = '\0';
        return makeResult(name);
}

FontName FontName::make(StringView familyText, uint16_t pixelSize, uint8_t styleBits) {
        FontName name;
        name.familyLength = static_cast<uint8_t>(familyText.size() < MaxFamily ? familyText.size() : MaxFamily);
        std::memcpy(name.family, familyText.data(), name.familyLength);
        name.family[name.familyLength] = '\0';
        name.size = pixelSize;
        name.style = styleBits;
        name.hasStyle = true;
        return name;
}

size_t FontName::format(char *out, size_t capacity) const {
        if (capacity == 0) return 0;
        const StringView first = role != Role::None ? StringView(roleName(role)) : familyName();
        size_t           n = 0;
        const auto       add = [&](StringView text) {
                for (char c : text)
                        if (n + 1 < capacity) out[n++] = c;
        };
        add(first);
        if (size != 0) {
                char      number[8];
                const int w = std::snprintf(number, sizeof(number), " %u", unsigned(size));
                add(StringView(number, w > 0 ? size_t(w) : 0));
        }
        if ((style & Bold) != 0) add(" bold");
        if ((style & Italic) != 0) add(" italic");
        if (hasStyle && style == 0 && role != Role::None) add(" regular");
        out[n] = '\0';
        return n;
}

const char *FontName::roleName(Role role) {
        const auto i = static_cast<size_t>(role);
        return i < sizeof(RoleWords) / sizeof(RoleWords[0]) ? RoleWords[i] : "";
}

FontName::Role FontName::roleFromName(StringView word) {
        for (size_t i = 1; i < sizeof(RoleWords) / sizeof(RoleWords[0]); ++i)
                if (sameWord(word, RoleWords[i])) return static_cast<Role>(i);
        return Role::None;
}

BMDS_NAMESPACE_END
