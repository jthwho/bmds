/**
 * @file      input.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Input's pure parts: the US keymap, boot keyboard reports as key events,
 * keys as terminal bytes.
 */

#include <doctest.h>

#include <string>
#include <vector>
#include <bmds/keyboarddecoder.h>
#include <bmds/terminalkeys.h>

using bmds::KeyboardDecoder;
using bmds::KeyEvent;
using bmds::Span;
namespace HidKey = bmds::HidKey;
namespace Keymap = bmds::Keymap;
namespace TerminalKeys = bmds::TerminalKeys;

namespace {

        using Action = KeyEvent::Action;

        struct Recorder {
                        std::vector<KeyEvent> events;

                        static void emit(void *arg, const KeyEvent &e) {
                                static_cast<Recorder *>(arg)->events.push_back(e);
                                return;
                        }
        };

        // A boot report: modifiers, the reserved byte, six keys.
        struct Report {
                        uint8_t bytes[8] = {};

                        Report(uint8_t modifiers, std::initializer_list<uint8_t> keys) {
                                bytes[0] = modifiers;
                                size_t i = 2;
                                for (const uint8_t k : keys) bytes[i++] = k;
                        }

                        Span<const uint8_t> span() const { return Span<const uint8_t>(bytes, sizeof(bytes)); }
        };

        KeyEvent key(uint8_t usage, uint8_t modifiers = 0, uint8_t locks = 0) {
                KeyEvent e;
                e.usage = usage;
                e.modifiers = modifiers;
                e.locks = locks;
                e.codepoint = Keymap::us(usage, modifiers, locks);
                return e;
        }

        std::string encode(const KeyEvent &e, bool application = false) {
                char         out[TerminalKeys::MaxBytes];
                const size_t n = TerminalKeys::encode(e, application, out);
                return std::string(out, n);
        }

} // namespace

TEST_CASE("Keymap: US letters, digits, punctuation, Shift and Caps Lock") {
        CHECK(Keymap::us(HidKey::A, 0, 0) == 'a');
        CHECK(Keymap::us(HidKey::Z, KeyEvent::LeftShift, 0) == 'Z');
        CHECK(Keymap::us(HidKey::A, 0, KeyEvent::CapsLock) == 'A');
        CHECK(Keymap::us(HidKey::A, KeyEvent::RightShift, KeyEvent::CapsLock) == 'a');
        CHECK(Keymap::us(HidKey::Digit1, 0, 0) == '1');
        CHECK(Keymap::us(HidKey::Digit0, 0, 0) == '0');
        CHECK(Keymap::us(HidKey::Digit0, KeyEvent::LeftShift, 0) == ')');
        CHECK(Keymap::us(HidKey::Digit1 + 1, KeyEvent::LeftShift, KeyEvent::CapsLock) == '@'); // Caps: letters only
        CHECK(Keymap::us(HidKey::Space, 0, 0) == ' ');
        CHECK(Keymap::us(0x38, KeyEvent::LeftShift, 0) == '?');
        CHECK(Keymap::us(0x34, KeyEvent::LeftShift, 0) == '"');
        CHECK(Keymap::us(0x35, 0, 0) == '`');
        CHECK(Keymap::us(HidKey::Enter, 0, 0) == 0);
        CHECK(Keymap::us(HidKey::Tab, 0, 0) == 0);
        CHECK(Keymap::us(HidKey::F1, 0, 0) == 0);
}

TEST_CASE("Keymap: the keypad's digits only with Num Lock") {
        CHECK(Keymap::us(HidKey::Keypad1, 0, 0) == 0);
        CHECK(Keymap::us(HidKey::Keypad1, 0, KeyEvent::NumLock) == '1');
        CHECK(Keymap::us(HidKey::Keypad0, 0, KeyEvent::NumLock) == '0');
        CHECK(Keymap::us(HidKey::KeypadPeriod, 0, KeyEvent::NumLock) == '.');
        CHECK(Keymap::us(HidKey::KeypadSlash, 0, 0) == '/'); // operators always
        CHECK(Keymap::us(HidKey::KeypadEnter, 0, KeyEvent::NumLock) == 0);
}

TEST_CASE("KeyboardDecoder: presses and releases from reports") {
        KeyboardDecoder d;
        Recorder        r;
        CHECK_FALSE(d.report(Report(0, {HidKey::A}).span(), 0, Recorder::emit, &r));
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].usage == HidKey::A);
        CHECK(r.events[0].action == Action::Press);
        CHECK(r.events[0].codepoint == 'a');

        // Shift down with B: the modifier first, then B shifted.
        r.events.clear();
        d.report(Report(KeyEvent::LeftShift, {HidKey::A, HidKey::A + 1}).span(), 10, Recorder::emit, &r);
        REQUIRE(r.events.size() == 2);
        CHECK(r.events[0].usage == 0xe1);
        CHECK(r.events[0].action == Action::Press);
        CHECK(r.events[1].usage == HidKey::A + 1);
        CHECK(r.events[1].codepoint == 'B');

        // All up: releases (the old modifiers), A before B as they were listed.
        r.events.clear();
        d.report(Report(0, {}).span(), 20, Recorder::emit, &r);
        REQUIRE(r.events.size() == 3);
        CHECK(r.events[0].usage == 0xe1);
        CHECK(r.events[0].action == Action::Release);
        CHECK(r.events[1].usage == HidKey::A);
        CHECK(r.events[1].action == Action::Release);
        CHECK(r.events[2].usage == HidKey::A + 1);
        CHECK(r.events[2].codepoint == 0);
}

TEST_CASE("KeyboardDecoder: rollover errors and short reports are ignored") {
        KeyboardDecoder d;
        Recorder        r;
        d.report(Report(0, {HidKey::A}).span(), 0, Recorder::emit, &r);
        r.events.clear();
        d.report(Report(0, {HidKey::ErrorRollOver, HidKey::ErrorRollOver}).span(), 1, Recorder::emit, &r);
        CHECK(r.events.empty());
        const uint8_t shortReport[2] = {0, 0};
        d.report(Span<const uint8_t>(shortReport, 2), 2, Recorder::emit, &r);
        CHECK(r.events.empty());
        d.report(Report(0, {}).span(), 3, Recorder::emit, &r); // A still down until now
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].action == Action::Release);
}

TEST_CASE("KeyboardDecoder: lock keys toggle, and say so") {
        KeyboardDecoder d;
        Recorder        r;
        CHECK(d.report(Report(0, {HidKey::CapsLock}).span(), 0, Recorder::emit, &r));
        CHECK(d.locks() == KeyEvent::CapsLock);
        CHECK_FALSE(d.report(Report(0, {}).span(), 1, Recorder::emit, &r));
        r.events.clear();
        d.report(Report(0, {HidKey::A}).span(), 2, Recorder::emit, &r);
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].codepoint == 'A');
        CHECK(r.events[0].locks == KeyEvent::CapsLock);
        CHECK(d.report(Report(0, {HidKey::A, HidKey::CapsLock}).span(), 3, Recorder::emit, &r));
        CHECK(d.locks() == 0);
        // A lock key doesn't repeat, nor take A's repeat over or restart it.
        r.events.clear();
        CHECK(d.repeat(2 + KeyboardDecoder::RepeatDelayMs, Recorder::emit, &r) == KeyboardDecoder::RepeatIntervalMs);
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].usage == HidKey::A);
        CHECK(r.events[0].codepoint == 'a'); // Caps Lock off again
}

TEST_CASE("KeyboardDecoder: the last key pressed repeats while held") {
        KeyboardDecoder d;
        Recorder        r;
        CHECK(d.repeat(0, Recorder::emit, &r) == KeyboardDecoder::NoRepeat);
        d.report(Report(0, {HidKey::A}).span(), 1000, Recorder::emit, &r);
        r.events.clear();
        CHECK(d.repeat(1000, Recorder::emit, &r) == KeyboardDecoder::RepeatDelayMs);
        CHECK(d.repeat(1499, Recorder::emit, &r) == 1);
        CHECK(r.events.empty());
        CHECK(d.repeat(1500, Recorder::emit, &r) == KeyboardDecoder::RepeatIntervalMs);
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].action == Action::Repeat);
        CHECK(r.events[0].codepoint == 'a');
        // Late by more than an interval: one repeat, the next a whole interval on.
        r.events.clear();
        CHECK(d.repeat(1700, Recorder::emit, &r) == KeyboardDecoder::RepeatIntervalMs);
        CHECK(r.events.size() == 1);
        // A second key takes over; releasing it stops the repeat.
        d.report(Report(0, {HidKey::A, HidKey::A + 1}).span(), 1710, Recorder::emit, &r);
        r.events.clear();
        d.repeat(1710 + KeyboardDecoder::RepeatDelayMs, Recorder::emit, &r);
        REQUIRE(r.events.size() == 1);
        CHECK(r.events[0].usage == HidKey::A + 1);
        d.report(Report(0, {HidKey::A}).span(), 2300, Recorder::emit, &r);
        CHECK(d.repeat(5000, Recorder::emit, &r) == KeyboardDecoder::NoRepeat);
}

TEST_CASE("KeyboardDecoder: releaseAll lets every key and modifier go") {
        KeyboardDecoder d;
        Recorder        r;
        d.report(Report(KeyEvent::LeftCtrl, {HidKey::A}).span(), 0, Recorder::emit, &r);
        r.events.clear();
        d.releaseAll(Recorder::emit, &r);
        REQUIRE(r.events.size() == 2);
        CHECK(r.events[0].usage == HidKey::A);
        CHECK(r.events[1].usage == 0xe0);
        CHECK(d.repeat(10'000, Recorder::emit, &r) == KeyboardDecoder::NoRepeat);
}

TEST_CASE("TerminalKeys: text, controls, Alt") {
        CHECK(encode(key(HidKey::A)) == "a");
        CHECK(encode(key(HidKey::A, KeyEvent::LeftShift)) == "A");
        CHECK(encode(key(HidKey::A + 2, KeyEvent::LeftCtrl)) == "\x03"); // Ctrl-C
        CHECK(encode(key(HidKey::Z, KeyEvent::RightCtrl | KeyEvent::LeftShift)) == "\x1a");
        CHECK(encode(key(0x2f, KeyEvent::LeftCtrl)) == "\x1b"); // Ctrl-[
        CHECK(encode(key(HidKey::Space, KeyEvent::LeftCtrl)) == std::string(1, '\0'));
        CHECK(encode(key(HidKey::A + 23, KeyEvent::LeftAlt)) == "\x1bx");
        CHECK(encode(key(HidKey::Enter)) == "\r");
        CHECK(encode(key(HidKey::KeypadEnter)) == "\r");
        CHECK(encode(key(HidKey::Backspace)) == "\x7f");
        CHECK(encode(key(HidKey::Backspace, KeyEvent::LeftCtrl)) == "\x08");
        CHECK(encode(key(HidKey::Tab)) == "\t");
        CHECK(encode(key(HidKey::Tab, KeyEvent::LeftShift)) == "\x1b[Z");
        CHECK(encode(key(HidKey::Escape)) == "\x1b");
        KeyEvent up = key(HidKey::A);
        up.action = Action::Release;
        CHECK(encode(up).empty());
        CHECK(encode(key(0xe1)).empty()); // a modifier alone
        CHECK(encode(key(HidKey::CapsLock)).empty());
        KeyEvent euro = key(HidKey::A);
        euro.codepoint = 0x20ac;
        CHECK(encode(euro) == "\xe2\x82\xac");
}

TEST_CASE("TerminalKeys: cursor, editing and function keys") {
        CHECK(encode(key(HidKey::Up)) == "\x1b[A");
        CHECK(encode(key(HidKey::Up), true) == "\x1bOA");
        CHECK(encode(key(HidKey::Left, KeyEvent::LeftCtrl)) == "\x1b[1;5D");
        CHECK(encode(key(HidKey::Right, KeyEvent::LeftCtrl), true) == "\x1b[1;5C"); // modified: never O
        CHECK(encode(key(HidKey::Home)) == "\x1b[H");
        CHECK(encode(key(HidKey::End), true) == "\x1bOF");
        CHECK(encode(key(HidKey::Insert)) == "\x1b[2~");
        CHECK(encode(key(HidKey::Delete)) == "\x1b[3~");
        CHECK(encode(key(HidKey::PageUp, KeyEvent::LeftShift)) == "\x1b[5;2~");
        CHECK(encode(key(HidKey::PageDown)) == "\x1b[6~");
        CHECK(encode(key(HidKey::F1)) == "\x1bOP");
        CHECK(encode(key(HidKey::F1 + 3)) == "\x1bOS");
        CHECK(encode(key(HidKey::F1 + 4)) == "\x1b[15~");
        CHECK(encode(key(HidKey::F12)) == "\x1b[24~");
        CHECK(encode(key(HidKey::F12, KeyEvent::LeftCtrl | KeyEvent::LeftShift)) == "\x1b[24;6~");
        CHECK(encode(key(HidKey::F1, KeyEvent::LeftAlt)) == "\x1b[1;3P");
}

TEST_CASE("TerminalKeys: the keypad without Num Lock is navigation") {
        CHECK(encode(key(HidKey::Keypad1 + 7)) == "\x1b[A"); // 8: up
        CHECK(encode(key(HidKey::Keypad1)) == "\x1b[F");     // 1: end
        CHECK(encode(key(HidKey::Keypad0)) == "\x1b[2~");
        CHECK(encode(key(HidKey::KeypadPeriod)) == "\x1b[3~");
        CHECK(encode(key(HidKey::Keypad1 + 4)).empty()); // 5: nothing
        CHECK(encode(key(HidKey::Keypad1 + 7, 0, KeyEvent::NumLock)) == "8");
}
