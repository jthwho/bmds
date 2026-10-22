/**
 * @file      console.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The text screen (namespace Console): clearing it, moving the cursor, QBasic's sixteen
 * colours, and keys read one at a time (key() waits, keyNow() doesn't, like INKEY$). VT
 * sequences on the terminal the script runs in; keys are read with the terminal in raw
 * mode, which input() and the end of the script undo.
 */

#include <errno.h>
#include <new>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "angel.h"

namespace angel {

        namespace {

                struct termios saved;
                bool           raw = false;
                bool           cursorHidden = false;
                bool           coloured = false;

                void out(const char *text) {
                        fputs(text, stdout);
                        fflush(stdout);
                        return;
                }

                // Keys one at a time, not echoed; Ctrl-C still stops the script.
                bool enterRaw() {
                        if (raw) return true;
                        if (!isatty(0) || tcgetattr(0, &saved) != 0) return false;
                        struct termios t = saved;
                        t.c_lflag &= ~tcflag_t(ICANON | ECHO);
                        t.c_iflag &= ~tcflag_t(ICRNL | IXON);
                        t.c_cc[VMIN] = 1;
                        t.c_cc[VTIME] = 0;
                        if (tcsetattr(0, TCSANOW, &t) != 0) return false;
                        raw = true;
                        return true;
                }

                // A byte of input within @p ms (-1: wait), or -1.
                int readByte(int ms) {
                        struct pollfd p = {0, POLLIN, 0};
                        int           r;
                        while ((r = poll(&p, 1, ms)) < 0 && errno == EINTR) {}
                        if (r <= 0) return -1;
                        unsigned char c;
                        ssize_t       n;
                        while ((n = read(0, &c, 1)) < 0 && errno == EINTR) {}
                        return n == 1 ? c : -1;
                }

                // An escape sequence's name (ESC already read): "Escape" alone.
                const char *escapeName() {
                        const int c1 = readByte(50);
                        if (c1 < 0) return "Escape";
                        if (c1 == 'O') {
                                switch (readByte(50)) {
                                        case 'P': return "F1";
                                        case 'Q': return "F2";
                                        case 'R': return "F3";
                                        case 'S': return "F4";
                                        case 'H': return "Home";
                                        case 'F': return "End";
                                        case 'A': return "Up";
                                        case 'B': return "Down";
                                        case 'C': return "Right";
                                        case 'D': return "Left";
                                        default: return "Escape";
                                }
                        }
                        if (c1 != '[') return "Escape";
                        int number = 0, c;
                        while ((c = readByte(50)) >= 0 && ((c >= '0' && c <= '9') || c == ';'))
                                if (c != ';')
                                        number = number * 10 + (c - '0');
                                else
                                        number = 0; // modifiers after ';' aren't told apart
                        switch (c) {
                                case 'A': return "Up";
                                case 'B': return "Down";
                                case 'C': return "Right";
                                case 'D': return "Left";
                                case 'H': return "Home";
                                case 'F': return "End";
                                case '~': break;
                                default: return "Escape";
                        }
                        switch (number) {
                                case 1:
                                case 7: return "Home";
                                case 2: return "Insert";
                                case 3: return "Delete";
                                case 4:
                                case 8: return "End";
                                case 5: return "PageUp";
                                case 6: return "PageDown";
                                case 11: return "F1";
                                case 12: return "F2";
                                case 13: return "F3";
                                case 14: return "F4";
                                case 15: return "F5";
                                case 17: return "F6";
                                case 18: return "F7";
                                case 19: return "F8";
                                case 20: return "F9";
                                case 21: return "F10";
                                case 23: return "F11";
                                case 24: return "F12";
                                default: return "Escape";
                        }
                }

                // The next key (waiting up to @p ms, -1: forever) as its name, or "" if none.
                void readKey(asIScriptGeneric *gen, int ms) {
                        fflush(stdout);
                        String *key = new (gen->GetAddressOfReturnLocation()) String();
                        if (!enterRaw()) {
                                // Not a terminal: characters as they come.
                                if (ms == 0) return;
                                const int c = getchar();
                                if (c != EOF) {
                                        const char ch = char(c);
                                        key->assign(&ch, 1);
                                }
                                return;
                        }
                        const int c = readByte(ms);
                        if (c < 0) return;
                        const char *name = nullptr;
                        if (c == 27)
                                name = escapeName();
                        else if (c == '\r' || c == '\n')
                                name = "Enter";
                        else if (c == 127 || c == 8)
                                name = "Backspace";
                        else if (c == '\t')
                                name = "Tab";
                        if (name != nullptr) {
                                key->assign(name, strlen(name));
                                return;
                        }
                        if (c < 32) {
                                char ctrl[8] = "Ctrl+A";
                                ctrl[5] = char('A' + c - 1);
                                key->assign(ctrl, 6);
                                return;
                        }
                        // A character: its whole UTF-8 sequence.
                        char bytes[4] = {char(c)};
                        int  n = 1, more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
                        while (more-- > 0) {
                                const int b = readByte(50);
                                if (b < 0) break;
                                bytes[n++] = char(b);
                        }
                        key->assign(bytes, size_t(n));
                }

                void key(asIScriptGeneric *gen) {
                        readKey(gen, -1);
                }
                void keyNow(asIScriptGeneric *gen) {
                        readKey(gen, 0);
                }
                void clear(asIScriptGeneric *) {
                        out("\033[2J\033[H");
                }
                void at(asIScriptGeneric *gen) {
                        const int x = int(gen->GetArgDWord(0)), y = int(gen->GetArgDWord(1));
                        char      seq[32];
                        snprintf(seq, sizeof(seq), "\033[%d;%dH", (y < 0 ? 0 : y) + 1, (x < 0 ? 0 : x) + 1);
                        out(seq);
                }
                // QBasic's colour numbers (0-15) as ANSI's.
                int ansi(int colour) {
                        static constexpr int order[8] = {0, 4, 2, 6, 1, 5, 3, 7};
                        return order[colour & 7] + ((colour & 8) != 0 ? 60 : 0);
                }
                void color(asIScriptGeneric *gen) {
                        const int fg = int(gen->GetArgDWord(0)), bg = int(gen->GetArgDWord(1));
                        char      seq[32];
                        if (bg >= 0)
                                snprintf(seq, sizeof(seq), "\033[%d;%dm", 30 + ansi(fg), 40 + ansi(bg));
                        else
                                snprintf(seq, sizeof(seq), "\033[%dm", 30 + ansi(fg));
                        coloured = true;
                        out(seq);
                }
                void reset(asIScriptGeneric *) {
                        coloured = false;
                        out("\033[0m");
                }
                void cursor(asIScriptGeneric *gen) {
                        cursorHidden = gen->GetArgByte(0) == 0;
                        out(cursorHidden ? "\033[?25l" : "\033[?25h");
                }
                void size(int &columns, int &rows) {
                        struct winsize w;
                        if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col != 0 && w.ws_row != 0) {
                                columns = w.ws_col;
                                rows = w.ws_row;
                                return;
                        }
                        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
                        columns = c != nullptr && atoi(c) > 0 ? atoi(c) : 80;
                        rows = l != nullptr && atoi(l) > 0 ? atoi(l) : 24;
                        return;
                }
                void width(asIScriptGeneric *gen) {
                        int columns, rows;
                        size(columns, rows);
                        gen->SetReturnDWord(asDWORD(columns));
                }
                void height(asIScriptGeneric *gen) {
                        int columns, rows;
                        size(columns, rows);
                        gen->SetReturnDWord(asDWORD(rows));
                }

        } // namespace

        void cookedTerminal() {
                if (raw) {
                        tcsetattr(0, TCSANOW, &saved);
                        raw = false;
                }
                return;
        }

        void restoreTerminal() {
                fflush(stdout);
                cookedTerminal();
                if (cursorHidden) out("\033[?25h");
                if (coloured) out("\033[0m");
                cursorHidden = false;
                coloured = false;
                return;
        }

        void registerConsole(asIScriptEngine *engine) {
                check(engine->SetDefaultNamespace("Console"), "namespace Console");
                check(engine->RegisterEnum("Color"), "Console::Color");
                static constexpr const char *names[16] = {"Black",    "Blue",         "Green",      "Cyan",
                                                          "Red",      "Magenta",      "Brown",      "LightGray",
                                                          "DarkGray", "LightBlue",    "LightGreen", "LightCyan",
                                                          "LightRed", "LightMagenta", "Yellow",     "White"};
                for (int i = 0; i < 16; ++i) check(engine->RegisterEnumValue("Color", names[i], i), names[i]);
                def(engine, "void clear()", clear, "Clears the text screen; the cursor goes to the top left.");
                def(engine, "void at(int x, int y)", at,
                    "Moves the cursor to column x, row y (from 0, 0 at the top left).");
                def(engine, "void color(int foreground, int background = -1)", color,
                    "The text's colour (QBasic's 0-15, Console::Yellow...), and the background's (-1: as it is).");
                def(engine, "void reset()", reset, "The usual colours again.");
                def(engine, "void cursor(bool visible)", cursor, "Shows or hides the cursor.");
                def(engine, "string key()", key,
                    "Waits for a key: a character (\"a\"), or a name (\"Enter\", \"Escape\", \"Up\", \"F1\", "
                    "\"Ctrl+A\"...).");
                def(engine, "string keyNow()", keyNow, "A key if one was pressed (as key()), or \"\" without waiting.");
                def(engine, "int width()", width, "The text screen's width in columns.");
                def(engine, "int height()", height, "The text screen's height in rows.");
                check(engine->SetDefaultNamespace(""), "namespace");
                return;
        }

} // namespace angel
