/**
 * @file      screenconsole.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The console on a screen in memory: output onto the screen, input pushed,
 * Ctrl-C, replies, the redraw hook.
 */

#include <doctest.h>

#include <string>
#include <bmds/interrupt.h>
#include <bmds/screenconsole.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::MemSpace;
using bmds::ScreenConsole;
using bmds::WritableByteSpan;

namespace {

        std::string rowText(ScreenConsole &console, uint16_t row) {
                ScreenConsole::Locked locked(console);
                std::string           s;
                for (uint16_t c = 0; c < locked.screen().columns(); ++c)
                        s += static_cast<char>(locked.screen().row(row)[c].codepoint);
                while (!s.empty() && s.back() == ' ') s.pop_back();
                return s;
        }

        ByteSpan bytes(const char *s) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(s), std::char_traits<char>::length(s));
        }

        std::string readAll(ScreenConsole &console) {
                std::string s;
                uint8_t     buf[64];
                for (;;) {
                        auto r = console.tryRead(WritableByteSpan(buf));
                        if (r.isError()) break;
                        s.append(reinterpret_cast<char *>(buf), r.value());
                }
                return s;
        }

        int  hooks = 0;
        void countHook(void *) {
                ++hooks;
                return;
        }

} // namespace

TEST_CASE("ScreenConsole: output onto the screen, \\n as CR LF, the size set") {
        ScreenConsole console("test", MemSpace::Psram);
        REQUIRE(console.resize(20, 4).isOk());
        CHECK(console.size().columns == 20);
        CHECK(console.size().rows == 4);
        CHECK(console.write(bytes("hello\nworld"), 0).value() == 11);
        CHECK(rowText(console, 0) == "hello");
        CHECK(rowText(console, 1) == "world");
        auto mode = console.mode();
        mode.nlToCrNl = false;
        console.setMode(mode);
        CHECK(console.write(bytes("\nX"), 0).isOk());
        CHECK(rowText(console, 2) == "     X"); // LF alone keeps the column
        ScreenConsole::Locked locked(console);
        CHECK(locked.screen().cursorRow() == 2);
}

TEST_CASE("ScreenConsole: input pushed is read; Ctrl-C interrupts and drops what came before") {
        ScreenConsole console("test", MemSpace::Psram);
        REQUIRE(console.resize(20, 4).isOk());
        CHECK(console.pushInput(bytes("ls\n")) == 3);
        CHECK(readAll(console) == "ls\n");
        CHECK(console.pushInput(bytes("abc\x03xyz")) == 7);
        CHECK(console.interrupt().isRaised());
        CHECK(readAll(console) == "xyz");
        console.interrupt().clear();
        auto mode = console.mode();
        mode.signals = false;
        console.setMode(mode);
        CHECK(console.pushInput(bytes("a\x03")) == 2);
        CHECK(!console.interrupt().isRaised());
        CHECK(readAll(console) == "a\x03");
        // Full: the rest is refused.
        std::string big(ScreenConsole::InputBytes + 10, 'q');
        CHECK(console.pushInput(bytes(big.c_str())) == ScreenConsole::InputBytes);
}

TEST_CASE("ScreenConsole: the screen's replies come back as input") {
        ScreenConsole console("test", MemSpace::Psram);
        REQUIRE(console.resize(20, 4).isOk());
        CHECK(console.write(bytes("ab\x1b[6n"), 0).isOk());
        CHECK(readAll(console) == "\x1b[1;3R");
}

TEST_CASE("ScreenConsole: the hook is asked once until the screen is drawn") {
        ScreenConsole console("test", MemSpace::Psram);
        REQUIRE(console.resize(20, 4).isOk());
        hooks = 0;
        console.setChangedHook(countHook, nullptr);
        CHECK(console.write(bytes("a"), 0).isOk());
        CHECK(console.write(bytes("b"), 0).isOk());
        CHECK(hooks == 1);
        console.drawn();
        CHECK(console.write(bytes("\x1b[H"), 0).isOk()); // the cursor moved, nothing else
        CHECK(hooks == 2);
        console.drawn();
        {
                ScreenConsole::Locked locked(console);
                locked.screen().clearDamage();
        }
        CHECK(console.write(bytes("\x1b[H"), 0).isOk()); // nothing changed
        CHECK(hooks == 2);
}
