/**
 * @file      chord.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Keys and chords as text (devplan/hotkeys.md §3.1, devplan/controls.md §7c).
 */

#include <cstdio>
#include <bmds/chord.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Keyboard = 0x07u << 16;
        constexpr uint32_t Consumer = 0x0cu << 16;
        constexpr uint32_t Desktop = 0x01u << 16;
        constexpr uint32_t LeftCtrl = Keyboard | 0xe0;

        struct Name {
                        const char *name;
                        uint32_t    usage;
        };

        // The named keys, beyond letters, digits and F1-F24 (made by rule). The first of a usage's
        // names is its canonical one.
        constexpr Name Names[] = {
                {"Enter", Keyboard | 0x28},
                {"Return", Keyboard | 0x28},
                {"Escape", Keyboard | 0x29},
                {"Esc", Keyboard | 0x29},
                {"Backspace", Keyboard | 0x2a},
                {"Tab", Keyboard | 0x2b},
                {"Space", Keyboard | 0x2c},
                {"Minus", Keyboard | 0x2d},
                {"Equals", Keyboard | 0x2e},
                {"LeftBracket", Keyboard | 0x2f},
                {"RightBracket", Keyboard | 0x30},
                {"Backslash", Keyboard | 0x31},
                {"Semicolon", Keyboard | 0x33},
                {"Apostrophe", Keyboard | 0x34},
                {"Grave", Keyboard | 0x35},
                {"Comma", Keyboard | 0x36},
                {"Period", Keyboard | 0x37},
                {"Slash", Keyboard | 0x38},
                {"CapsLock", Keyboard | 0x39},
                {"PrintScreen", Keyboard | 0x46},
                {"ScrollLock", Keyboard | 0x47},
                {"Pause", Keyboard | 0x48},
                {"Insert", Keyboard | 0x49},
                {"Home", Keyboard | 0x4a},
                {"PageUp", Keyboard | 0x4b},
                {"Delete", Keyboard | 0x4c},
                {"End", Keyboard | 0x4d},
                {"PageDown", Keyboard | 0x4e},
                {"Right", Keyboard | 0x4f},
                {"Left", Keyboard | 0x50},
                {"Down", Keyboard | 0x51},
                {"Up", Keyboard | 0x52},
                {"NumLock", Keyboard | 0x53},
                {"KeypadSlash", Keyboard | 0x54},
                {"KeypadAsterisk", Keyboard | 0x55},
                {"KeypadMinus", Keyboard | 0x56},
                {"KeypadPlus", Keyboard | 0x57},
                {"KeypadEnter", Keyboard | 0x58},
                {"Keypad1", Keyboard | 0x59},
                {"Keypad2", Keyboard | 0x5a},
                {"Keypad3", Keyboard | 0x5b},
                {"Keypad4", Keyboard | 0x5c},
                {"Keypad5", Keyboard | 0x5d},
                {"Keypad6", Keyboard | 0x5e},
                {"Keypad7", Keyboard | 0x5f},
                {"Keypad8", Keyboard | 0x60},
                {"Keypad9", Keyboard | 0x61},
                {"Keypad0", Keyboard | 0x62},
                {"KeypadPeriod", Keyboard | 0x63},
                {"Menu", Keyboard | 0x65},
                {"LeftCtrl", Keyboard | 0xe0},
                {"LeftShift", Keyboard | 0xe1},
                {"LeftAlt", Keyboard | 0xe2},
                {"LeftWin", Keyboard | 0xe3},
                {"RightCtrl", Keyboard | 0xe4},
                {"RightShift", Keyboard | 0xe5},
                {"RightAlt", Keyboard | 0xe6},
                {"RightWin", Keyboard | 0xe7},
                {"VolUp", Consumer | 0xe9},
                {"VolDown", Consumer | 0xea},
                {"Mute", Consumer | 0xe2},
                {"PlayPause", Consumer | 0xcd},
                {"Next", Consumer | 0xb5},
                {"Prev", Consumer | 0xb6},
                {"Stop", Consumer | 0xb7},
                {"Calculator", Consumer | 0x192},
                {"Power", Desktop | 0x81},
                {"Sleep", Desktop | 0x82},
        };

        // The side-less modifiers, as modifiers and as keys (the left one's usage).
        constexpr Name Modifiers[] = {
                {"Ctrl", Chord::Ctrl}, {"Shift", Chord::Shift}, {"Alt", Chord::Alt}, {"Win", Chord::Win}};

        char lower(char c) {
                return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        bool same(StringView a, const char *b) {
                size_t i = 0;
                for (; i < a.size(); ++i)
                        if (b[i] == '\0' || lower(a[i]) != lower(b[i])) return false;
                return b[i] == '\0';
        }

        // A key's name: the table's, a letter, a digit, F1-F24, or hex.
        bool keyOf(StringView text, uint32_t &usage, bool &eitherSide) {
                eitherSide = false;
                for (size_t i = 0; i < sizeof(Modifiers) / sizeof(Modifiers[0]); ++i)
                        if (same(text, Modifiers[i].name)) {
                                usage = LeftCtrl + uint32_t(i);
                                eitherSide = true;
                                return true;
                        }
                for (const Name &n : Names)
                        if (same(text, n.name)) {
                                usage = n.usage;
                                return true;
                        }
                if (text.size() == 1) {
                        const char c = lower(text[0]);
                        if (c >= 'a' && c <= 'z') {
                                usage = Keyboard | uint32_t(0x04 + c - 'a');
                                return true;
                        }
                        if (c >= '1' && c <= '9') {
                                usage = Keyboard | uint32_t(0x1e + c - '1');
                                return true;
                        }
                        if (c == '0') {
                                usage = Keyboard | 0x27;
                                return true;
                        }
                }
                if ((text.size() == 2 || text.size() == 3) && lower(text[0]) == 'f') {
                        uint32_t n = 0;
                        for (size_t i = 1; i < text.size(); ++i) {
                                if (text[i] < '0' || text[i] > '9') return false;
                                n = n * 10 + uint32_t(text[i] - '0');
                        }
                        if (n >= 1 && n <= 12) usage = Keyboard | (0x3a + n - 1);
                        if (n >= 13 && n <= 24) usage = Keyboard | (0x68 + n - 13);
                        return n >= 1 && n <= 24;
                }
                if (text.size() == 10 && text[0] == '0' && lower(text[1]) == 'x') {
                        uint32_t v = 0;
                        for (size_t i = 2; i < 10; ++i) {
                                const char c = lower(text[i]);
                                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                                if (d < 0) return false;
                                v = v << 4 | uint32_t(d);
                        }
                        usage = v;
                        return v != 0;
                }
                return false;
        }

} // namespace

bool Chord::nameOf(uint32_t usage, char (&out)[MaxName]) {
        for (const Name &n : Names)
                if (n.usage == usage) {
                        std::snprintf(out, sizeof(out), "%s", n.name);
                        return true;
                }
        const uint32_t u = usage & 0xffffu;
        if ((usage >> 16) != 0x07) {
                return false;
        } else if (u >= 0x04 && u <= 0x1d) {
                std::snprintf(out, sizeof(out), "%c", char('A' + u - 0x04));
        } else if (u >= 0x1e && u <= 0x27) {
                std::snprintf(out, sizeof(out), "%c", u == 0x27 ? '0' : char('1' + u - 0x1e));
        } else if (u >= 0x3a && u <= 0x45) {
                std::snprintf(out, sizeof(out), "F%u", unsigned(u - 0x3a + 1));
        } else if (u >= 0x68 && u <= 0x73) {
                std::snprintf(out, sizeof(out), "F%u", unsigned(u - 0x68 + 13));
        } else {
                return false;
        }
        return true;
}

Error Chord::parse(StringView text, Chord &out) {
        Chord  c;
        size_t at = 0;
        text = text.trimmed();
        if (text.isEmpty()) return Error::Invalid;
        for (;;) {
                size_t plus = text.indexOf('+', at);
                // A '+' as the key itself is never a name here: "Ctrl+" is invalid.
                if (plus == StringView::NPos) plus = text.size();
                const StringView part = text.mid(at, plus - at).trimmed();
                if (plus == text.size()) {
                        if (!keyOf(part, c.usage, c.eitherSide)) return Error::Invalid;
                        break;
                }
                bool found = false;
                for (const Name &m : Modifiers)
                        if (same(part, m.name)) {
                                c.modifiers |= uint8_t(m.usage);
                                found = true;
                        }
                if (!found) return Error::Invalid;
                at = plus + 1;
        }
        out = c;
        return Error::Ok;
}

size_t Chord::format(char *out, size_t size) const {
        if (size == 0) return 0;
        size_t used = 0;
        out[0] = '\0';
        auto add = [&](const char *s) {
                const int w = std::snprintf(out + used, size - used, "%s", s);
                if (w > 0) used = used + size_t(w) < size ? used + size_t(w) : size - 1;
        };
        for (const Name &m : Modifiers)
                if ((modifiers & m.usage) != 0) {
                        add(m.name);
                        add("+");
                }
        char name[MaxName];
        if (eitherSide && usage >= LeftCtrl && usage < LeftCtrl + 4) {
                add(Modifiers[usage - LeftCtrl].name);
        } else if (nameOf(usage, name)) {
                add(name);
        } else {
                char hex[12];
                std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(usage));
                add(hex);
        }
        return used;
}

uint8_t Chord::modifiersOf(uint8_t m) {
        return uint8_t(((m & 0x11) != 0 ? Ctrl : 0) | ((m & 0x22) != 0 ? Shift : 0) | ((m & 0x44) != 0 ? Alt : 0) |
                       ((m & 0x88) != 0 ? Win : 0));
}

bool Chord::matches(uint32_t key, uint8_t down) const {
        const bool same = key == usage || (eitherSide && key == usage + 4);
        if (!same) return false;
        // A modifier key's own bit is down with it: not one of the chord's modifiers.
        if ((key >> 16) == 0x07 && (key & 0xffffu) >= 0xe0 && (key & 0xffffu) <= 0xe7)
                down = uint8_t(down & ~(1u << ((key & 0xffffu) - 0xe0) % 4));
        return (down & modifiers) == modifiers;
}

BMDS_NAMESPACE_END
