/**
 * @file      terminalkeys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Keys as the bytes an xterm sends (devplan/input.md §4).
 */

#include <bmds/terminalkeys.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr char Esc = '\x1b';

        // xterm's modifier parameter: 1 + Shift (1) + Alt (2) + Ctrl (4); 1 means none.
        unsigned modifierParameter(const KeyEvent &e) {
                return 1u + (e.hasShift() ? 1u : 0u) + (e.hasAlt() ? 2u : 0u) + (e.hasCtrl() ? 4u : 0u);
        }

        // Appends a decimal number (1-99).
        size_t number(char *out, unsigned n) {
                if (n >= 10) {
                        out[0] = static_cast<char>('0' + n / 10);
                        out[1] = static_cast<char>('0' + n % 10);
                        return 2;
                }
                out[0] = static_cast<char>('0' + n);
                return 1;
        }

        // A cursor-style key: ESC [ x, ESC O x (application mode), or ESC [ 1 ; m x.
        size_t letterKey(const KeyEvent &e, char final, bool application, char *out) {
                const unsigned m = modifierParameter(e);
                size_t         n = 0;
                out[n++] = Esc;
                if (m != 1) {
                        out[n++] = '[';
                        out[n++] = '1';
                        out[n++] = ';';
                        n += number(out + n, m);
                } else {
                        out[n++] = application ? 'O' : '[';
                }
                out[n++] = final;
                return n;
        }

        // An editing or function key: ESC [ code ~, or ESC [ code ; m ~.
        size_t tildeKey(const KeyEvent &e, unsigned code, char *out) {
                const unsigned m = modifierParameter(e);
                size_t         n = 0;
                out[n++] = Esc;
                out[n++] = '[';
                n += number(out + n, code);
                if (m != 1) {
                        out[n++] = ';';
                        n += number(out + n, m);
                }
                out[n++] = '~';
                return n;
        }

        // A character with Ctrl down, as xterm sends it; -1: the character itself.
        int controlOf(uint32_t c) {
                if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a' + 1);
                if (c >= '@' && c <= '_') return static_cast<int>(c - '@'); // @ A..Z [ \ ] ^ _
                switch (c) {
                        case ' ':
                        case '2': return 0x00;
                        case '3': return 0x1b;
                        case '4': return 0x1c;
                        case '5': return 0x1d;
                        case '6': return 0x1e;
                        case '7':
                        case '-': return 0x1f;
                        case '8':
                        case '?': return 0x7f;
                        default: return -1;
                }
        }

        // Code points of ESC [ n ~ keys, and F5..F12's.
        constexpr unsigned FunctionCodes[8] = {15, 17, 18, 19, 20, 21, 23, 24}; // F5..F12

} // namespace

size_t TerminalKeys::encode(const KeyEvent &e, bool applicationCursor, char (&out)[MaxBytes]) {
        if (e.action == KeyEvent::Action::Release) return 0;
        const uint8_t u = e.usage;

        // The keypad without Num Lock: the keys printed on it.
        uint8_t key = u;
        if (e.codepoint == 0 && u >= HidKey::Keypad1 && u <= HidKey::KeypadPeriod) {
                constexpr uint8_t Nav[11] = {HidKey::End,    HidKey::Down,   HidKey::PageDown, HidKey::Left,
                                             HidKey::None,   HidKey::Right,  HidKey::Home,     HidKey::Up,
                                             HidKey::PageUp, HidKey::Insert, HidKey::Delete};
                key = Nav[u - HidKey::Keypad1];
        }

        switch (key) {
                case HidKey::Up: return letterKey(e, 'A', applicationCursor, out);
                case HidKey::Down: return letterKey(e, 'B', applicationCursor, out);
                case HidKey::Right: return letterKey(e, 'C', applicationCursor, out);
                case HidKey::Left: return letterKey(e, 'D', applicationCursor, out);
                case HidKey::Home: return letterKey(e, 'H', applicationCursor, out);
                case HidKey::End: return letterKey(e, 'F', applicationCursor, out);
                case HidKey::Insert: return tildeKey(e, 2, out);
                case HidKey::Delete: return tildeKey(e, 3, out);
                case HidKey::PageUp: return tildeKey(e, 5, out);
                case HidKey::PageDown: return tildeKey(e, 6, out);
                default: break;
        }
        if (key >= HidKey::F1 && key <= HidKey::F12) {
                const unsigned f = key - HidKey::F1; // 0 = F1
                if (f < 4) return letterKey(e, static_cast<char>('P' + f), true, out);
                return tildeKey(e, FunctionCodes[f - 4], out);
        }

        size_t n = 0;
        if (e.hasAlt()) out[n++] = Esc; // Alt: Escape first ("meta sends escape")
        switch (key) {
                case HidKey::Enter:
                case HidKey::KeypadEnter: out[n++] = '\r'; return n;
                case HidKey::Escape: out[n++] = Esc; return n;
                case HidKey::Backspace: out[n++] = e.hasCtrl() ? '\x08' : '\x7f'; return n;
                case HidKey::Tab:
                        if (e.hasShift()) {
                                out[n++] = Esc;
                                out[n++] = '[';
                                out[n++] = 'Z';
                        } else {
                                out[n++] = '\t';
                        }
                        return n;
                default: break;
        }
        if (e.codepoint == 0) return 0; // a modifier, a lock key, an unmapped key
        if (e.hasCtrl()) {
                const int c = controlOf(e.codepoint);
                if (c >= 0) {
                        out[n++] = static_cast<char>(c);
                        return n;
                }
        }
        char         utf8[4];
        const size_t len = Utf8::encode(e.codepoint, utf8);
        for (size_t i = 0; i < len; ++i) out[n++] = utf8[i];
        return n;
}

BMDS_NAMESPACE_END
