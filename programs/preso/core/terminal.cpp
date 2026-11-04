/**
 * @file      terminal.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "terminal.h"

namespace preso {

        namespace {

                constexpr uint32_t Page7 = 0x70000;

                Event keyOf(uint32_t usage, uint32_t character) {
                        Event e;
                        e.type = Event::Type::Key;
                        e.key = usage != 0 ? (Page7 | usage) : 0;
                        e.character = character;
                        return e;
                }

                // A plain byte's key: letters and digits by their usages, a few others by theirs.
                Event plain(char c) {
                        uint32_t u = uint8_t(c);
                        if (u >= 'a' && u <= 'z') return keyOf(0x04 + (u - 'a'), u);
                        if (u >= 'A' && u <= 'Z') return keyOf(0x04 + (u - 'A'), u);
                        if (u >= '1' && u <= '9') return keyOf(0x1E + (u - '1'), u);
                        switch (u) {
                                case '0': return keyOf(0x27, u);
                                case '\r':
                                case '\n': return keyOf(0x28, 0);
                                case '\t': return keyOf(0x2B, 0);
                                case 0x7F:
                                case 0x08: return keyOf(0x2A, 0);
                                case ' ': return keyOf(0x2C, ' ');
                                case '.': return keyOf(0x37, '.');
                                case 0x03: return keyOf(0, 3); // Ctrl-C (terminalAction's Quit)
                                default: return keyOf(0, u >= 0x20 ? u : 0);
                        }
                }

        } // namespace

        bool TerminalKeys::push(char c, Event *key) {
                uint8_t b = uint8_t(c);
                // A character of several bytes (UTF-8): its character, no usage.
                if (_utf8Left > 0) {
                        if ((b & 0xC0) != 0x80) {
                                _utf8Left = 0; // broken: this byte as itself
                        } else {
                                _utf8 = (_utf8 << 6) | (b & 0x3F);
                                if (--_utf8Left > 0) return false;
                                *key = keyOf(0, _utf8);
                                return true;
                        }
                }
                if (_sequenceLength > 0) {
                        // ESC [ or ESC O, then parameters, then a final byte.
                        if (_sequenceLength == 1) {
                                // Escape twice: Escape (a lone one can't be told from a sequence's start).
                                if (b == 0x1B) {
                                        _sequenceLength = 0;
                                        *key = keyOf(0x29, 0);
                                        return true;
                                }
                                if (b == '[' || b == 'O') {
                                        _sequence[_sequenceLength++] = char(b);
                                        return false;
                                }
                                // ESC and a key (Alt with it, or a lone Escape): the key alone.
                                _sequenceLength = 0;
                                return push(c, key);
                        }
                        if (_sequenceLength < sizeof _sequence) _sequence[_sequenceLength++] = char(b);
                        if (b >= 0x40 && b <= 0x7E) {
                                uint32_t n = _sequenceLength;
                                _sequenceLength = 0;
                                // The number before `~`, if any.
                                uint32_t number = 0;
                                for (uint32_t i = 2; i + 1 < n; i++) {
                                        if (_sequence[i] >= '0' && _sequence[i] <= '9') {
                                                number = number * 10 + uint32_t(_sequence[i] - '0');
                                        } else {
                                                break;
                                        }
                                }
                                uint32_t usage = 0;
                                switch (b) {
                                        case 'A': usage = 0x52; break; // Up
                                        case 'B': usage = 0x51; break; // Down
                                        case 'C': usage = 0x4F; break; // Right
                                        case 'D': usage = 0x50; break; // Left
                                        case 'H': usage = 0x4A; break; // Home
                                        case 'F': usage = 0x4D; break; // End
                                        case 'Z': usage = 0x2B; break; // Shift+Tab: as Tab
                                        case '~':
                                                usage = number == 1 || number == 7   ? 0x4A
                                                        : number == 4 || number == 8 ? 0x4D
                                                        : number == 5                ? 0x4B
                                                        : number == 6                ? 0x4E
                                                        : number == 3                ? 0x4C
                                                                                     : 0;
                                                break;
                                        default: break;
                                }
                                if (usage == 0) return false; // a sequence of no key here
                                *key = keyOf(usage, 0);
                                return true;
                        }
                        if (_sequenceLength >= sizeof _sequence) _sequenceLength = 0; // too long: given up
                        return false;
                }
                if (b == 0x1B) {
                        _sequence[0] = char(b);
                        _sequenceLength = 1;
                        return false;
                }
                if (b >= 0xC0 && b < 0xF8) {
                        _utf8Left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
                        _utf8 = b & (b >= 0xF0 ? 0x07 : b >= 0xE0 ? 0x0F : 0x1F);
                        return false;
                }
                *key = plain(c);
                return true;
        }

        Action terminalAction(const Event &key, InputMode mode) {
                uint32_t usage = key.key & 0xFFFF;
                uint32_t c = key.character;
                if (usage == 0 && c == 3) return Action::Quit; // Ctrl-C
                if (mode == InputMode::Deck) {
                        switch (c) {
                                case 'n':
                                case 'j': return Action::Next;
                                case 'p':
                                case 'k': return Action::Previous;
                                case 'g': return Action::First;
                                case 'G': return Action::Last;
                                case 'q': return Action::Quit;
                                case 'o': return Action::Overview;
                                case 'i': return Action::Into;
                                case 'c': return Action::ClearInk;
                                case 'b':
                                case '.': return Action::Black;
                                case 'w': return Action::White;
                                default: break;
                        }
                        // Escape alone: nothing (it would quit, and a laptop's is easily touched).
                        if (usage == 0x29) return Action::None;
                        // Other letters are plain keys, whatever their usage maps to on the screen.
                        if (c != 0 && usage >= 0x04 && usage <= 0x1D) return Action::None;
                }
                if (mode == InputMode::Overview && (c == 'q' || c == 'o')) return Action::Close;
                return usage != 0 ? defaultAction(key.key, mode) : Action::None;
        }

} // namespace preso
