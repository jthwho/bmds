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
#include <bmds/chord.h>
#include <bmds/controls.h>
#include <bmds/keyboarddecoder.h>
#include <bmds/mouselayout.h>
#include <bmds/padlayout.h>
#include <bmds/padmapping.h>
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

namespace {

        // A classic wheel mouse: 3 buttons, padding, X, Y and the wheel, a byte each, no report ID.
        const uint8_t WheelMouse[] = {
                0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19,
                0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
                0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09,
                0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
        };

        // A receiver's: a keyboard (ID 1), then a mouse (ID 2) with 16 buttons, 12-bit X and Y, a
        // wheel and AC Pan.
        const uint8_t ReceiverMouse[] = {
                // The keyboard.
                0x05,
                0x01,
                0x09,
                0x06,
                0xa1,
                0x01,
                0x85,
                0x01,
                0x05,
                0x07,
                0x19,
                0xe0,
                0x29,
                0xe7,
                0x15,
                0x00,
                0x25,
                0x01,
                0x75,
                0x01,
                0x95,
                0x08,
                0x81,
                0x02,
                0x95,
                0x01,
                0x75,
                0x08,
                0x81,
                0x03,
                0x95,
                0x06,
                0x75,
                0x08,
                0x15,
                0x00,
                0x26,
                0xff,
                0x00,
                0x05,
                0x07,
                0x19,
                0x00,
                0x2a,
                0xff,
                0x00,
                0x81,
                0x00,
                0xc0,
                // The mouse.
                0x05,
                0x01,
                0x09,
                0x02,
                0xa1,
                0x01,
                0x85,
                0x02,
                0x09,
                0x01,
                0xa1,
                0x00,
                0x05,
                0x09,
                0x19,
                0x01,
                0x29,
                0x10,
                0x15,
                0x00,
                0x25,
                0x01,
                0x95,
                0x10,
                0x75,
                0x01,
                0x81,
                0x02,
                0x05,
                0x01,
                0x16,
                0x01,
                0xf8,
                0x26,
                0xff,
                0x07,
                0x75,
                0x0c,
                0x95,
                0x02,
                0x09,
                0x30,
                0x09,
                0x31,
                0x81,
                0x06,
                0x15,
                0x81,
                0x25,
                0x7f,
                0x75,
                0x08,
                0x95,
                0x01,
                0x09,
                0x38,
                0x81,
                0x06,
                0x05,
                0x0c,
                0x0a,
                0x38,
                0x02,
                0x95,
                0x01,
                0x81,
                0x06,
                0xc0,
                0xc0,
        };

        bmds::MouseLayout layoutOf(const uint8_t *d, size_t n) {
                return bmds::MouseLayout::parse(Span<const uint8_t>(d, n));
        }

} // namespace

TEST_CASE("MouseLayout: a classic wheel mouse") {
        const bmds::MouseLayout m = layoutOf(WheelMouse, sizeof(WheelMouse));
        REQUIRE(m.isValid());
        CHECK(m.reportId() == 0);
        CHECK(m.hasWheel());
        CHECK(m.buttonCount() == 3);
        const uint8_t      report[] = {0x05, 0xfe, 0x03, 0x01};
        bmds::PointerEvent e;
        REQUIRE(m.decode(Span<const uint8_t>(report, 4), e));
        CHECK(e.buttons == (bmds::PointerEvent::Left | bmds::PointerEvent::Middle));
        CHECK(e.dx == -2);
        CHECK(e.dy == 3);
        CHECK(e.wheel == 1);
        const uint8_t down[] = {0x00, 0x00, 0x00, 0xff};
        REQUIRE(m.decode(Span<const uint8_t>(down, 4), e));
        CHECK(e.wheel == -1);
        CHECK(!m.decode(Span<const uint8_t>(report, 2), e)); // too short for Y
}

TEST_CASE("MouseLayout: a receiver's mouse after its keyboard, 12-bit motion, by report ID") {
        const bmds::MouseLayout m = layoutOf(ReceiverMouse, sizeof(ReceiverMouse));
        REQUIRE(m.isValid());
        CHECK(m.reportId() == 2);
        CHECK(m.hasWheel());
        CHECK(m.buttonCount() == 8); // 16 on the device, 8 kept
        // X -5 and Y 300 in 12 bits each (0x12cffb), the wheel down, the right button.
        const uint8_t      report[] = {0x02, 0x02, 0x00, 0xfb, 0xcf, 0x12, 0xff, 0x00};
        bmds::PointerEvent e;
        REQUIRE(m.decode(Span<const uint8_t>(report, sizeof(report)), e));
        CHECK(e.buttons == bmds::PointerEvent::Right);
        CHECK(e.dx == -5);
        CHECK(e.dy == 300);
        CHECK(e.wheel == -1);
        const uint8_t keys[] = {0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0};
        CHECK(!m.decode(Span<const uint8_t>(keys, sizeof(keys)), e)); // the keyboard's report
}

TEST_CASE("MouseLayout: extended usages, a keyboard alone, truncated and random descriptors") {
        // The wheel mouse with its X as a 4-byte usage (the page in its high half).
        uint8_t extended[sizeof(WheelMouse) + 3];
        size_t  n = 0;
        for (size_t i = 0; i < sizeof(WheelMouse); ++i) {
                if (WheelMouse[i] == 0x09 && WheelMouse[i + 1] == 0x30) {
                        const uint8_t x[] = {0x0b, 0x30, 0x00, 0x01, 0x00};
                        for (const uint8_t b : x) extended[n++] = b;
                        ++i;
                        continue;
                }
                extended[n++] = WheelMouse[i];
        }
        REQUIRE(n == sizeof(extended));
        const bmds::MouseLayout m = layoutOf(extended, n);
        REQUIRE(m.isValid());
        const uint8_t      report[] = {0x00, 0x07, 0xf9, 0x00};
        bmds::PointerEvent e;
        REQUIRE(m.decode(Span<const uint8_t>(report, 4), e));
        CHECK(e.dx == 7);
        CHECK(e.dy == -7);
        // A keyboard has no mouse.
        CHECK(!layoutOf(ReceiverMouse, 49).isValid());
        // Cut anywhere: no harm, and no mouse until X and Y are in.
        for (size_t cut = 0; cut < sizeof(WheelMouse); ++cut) CHECK(layoutOf(WheelMouse, cut).isValid() == (cut >= 50));
        // Random bytes: parsed and decoded without reading past either.
        uint32_t state = 99;
        for (int t = 0; t < 20000; ++t) {
                uint8_t bytes[64];
                for (uint8_t &b : bytes) {
                        state = state * 1103515245u + 12345u;
                        b = static_cast<uint8_t>(state >> 16);
                }
                const bmds::MouseLayout r = layoutOf(bytes, 1 + t % 64);
                if (r.isValid()) (void)r.decode(Span<const uint8_t>(bytes, t % 16), e);
        }
}

namespace {

        // DragonRise's generic chip (0079:0011), an NES-style pad's (read off the board).
        constexpr uint8_t DragonRise[] = {
                0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0xa1, 0x02, 0x75, 0x08, 0x95, 0x05, 0x15, 0x00, 0x26, 0xff, 0x00,
                0x35, 0x00, 0x46, 0xff, 0x00, 0x09, 0x30, 0x09, 0x30, 0x09, 0x30, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
                0x75, 0x04, 0x95, 0x01, 0x25, 0x07, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x09, 0x00, 0x81, 0x42, 0x65, 0x00,
                0x75, 0x01, 0x95, 0x0a, 0x25, 0x01, 0x45, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0a, 0x81, 0x02, 0x06,
                0x00, 0xff, 0x75, 0x01, 0x95, 0x0a, 0x25, 0x01, 0x45, 0x01, 0x09, 0x01, 0x81, 0x02, 0xc0, 0xa1, 0x02,
                0x75, 0x08, 0x95, 0x04, 0x46, 0xff, 0x00, 0x26, 0xff, 0x00, 0x09, 0x02, 0x91, 0x02, 0xc0, 0xc0,
        };

        // A typical DirectInput pad: report ID 1, 12 buttons, a hat (0-7, null 8), X Y Z Rz.
        constexpr uint8_t HatPad[] = {
                0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x15,
                0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00,
                0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09,
                0x35, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0xc0,
        };

        bmds::PadLayout padOf(const uint8_t *d, size_t n) {
                return bmds::PadLayout::parse(Span<const uint8_t>(d, n));
        }

        // A DragonRise report: axes 0-4, then the null hat nibble and ten buttons.
        void dragonRiseReport(uint8_t (&r)[8], uint8_t x, uint8_t y, uint16_t buttons) {
                r[0] = r[1] = r[2] = 0x7f;
                r[3] = x;
                r[4] = y;
                r[5] = static_cast<uint8_t>(0x0f | (buttons & 0x0f) << 4);
                r[6] = static_cast<uint8_t>(buttons >> 4);
                r[7] = 0;
                return;
        }

} // namespace

TEST_CASE("PadLayout: DragonRise's pad, its repeated X axes numbered by position") {
        const bmds::PadLayout l = padOf(DragonRise, sizeof(DragonRise));
        REQUIRE(l.isValid());
        CHECK(l.reportId() == 0);
        CHECK(l.axisCount() == 5);
        CHECK(l.hatCount() == 0); // its nibble's usage is undefined
        CHECK(l.buttonCount() == 10);
        CHECK(l.axisUsage(3) == 0x30);
        CHECK(l.axisUsage(4) == 0x31);
        uint8_t r[8];
        dragonRiseReport(r, 0x00, 0xff, 1u << 2 | 1u << 9);
        bmds::RawPad raw;
        REQUIRE(l.decode(Span<const uint8_t>(r, sizeof(r)), raw));
        CHECK(raw.axes[0] == -129);
        CHECK(raw.axes[3] == -32768);
        CHECK(raw.axes[4] == 32767);
        CHECK(raw.buttons == (1u << 2 | 1u << 9));
        CHECK(!l.decode(Span<const uint8_t>(r, 6), raw)); // too short for its buttons
        CHECK(!bmds::MouseLayout::parse(Span<const uint8_t>(DragonRise, sizeof(DragonRise))).isValid());
        CHECK(!padOf(WheelMouse, sizeof(WheelMouse)).isValid());
}

TEST_CASE("PadLayout: a report ID, a hat with a null state") {
        const bmds::PadLayout l = padOf(HatPad, sizeof(HatPad));
        REQUIRE(l.isValid());
        CHECK(l.reportId() == 1);
        CHECK(l.axisCount() == 4);
        CHECK(l.hatCount() == 1);
        CHECK(l.buttonCount() == 12);
        bmds::RawPad  raw;
        const uint8_t r[] = {0x01, 0x01, 0x30, 0x80, 0x80, 0x00, 0xff}; // button 1, hat 3 (down-right)
        REQUIRE(l.decode(Span<const uint8_t>(r, sizeof(r)), raw));
        CHECK(raw.buttons == 1u);
        CHECK(raw.hats[0] == (bmds::RawPad::HatDown | bmds::RawPad::HatRight));
        CHECK(raw.axes[0] == 128);
        CHECK(raw.axes[2] == -32768);
        CHECK(raw.axes[3] == 32767);
        const uint8_t centred[] = {0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80}; // hat 8: its null state
        REQUIRE(l.decode(Span<const uint8_t>(centred, sizeof(centred)), raw));
        CHECK(raw.hats[0] == 0);
        const uint8_t other[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        CHECK(!l.decode(Span<const uint8_t>(other, sizeof(other)), raw));
        // Random bytes and every cut: no harm.
        for (size_t cut = 0; cut <= sizeof(HatPad); ++cut) (void)padOf(HatPad, cut);
        uint32_t state = 7;
        for (int t = 0; t < 20000; ++t) {
                uint8_t bytes[64];
                for (uint8_t &b : bytes) {
                        state = state * 1103515245u + 12345u;
                        b = static_cast<uint8_t>(state >> 16);
                }
                const bmds::PadLayout p = padOf(bytes, 1 + t % 64);
                if (p.isValid()) (void)p.decode(Span<const uint8_t>(bytes, t % 16), raw);
        }
}

TEST_CASE("PadMapping: the usual order, by usage") {
        const bmds::PadMapping m = bmds::PadMapping::standard(padOf(HatPad, sizeof(HatPad)));
        char                   text[bmds::PadMapping::MaxText];
        m.format(text, sizeof(text));
        CHECK(std::string(text) == "a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,"
                                   "leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,"
                                   "rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,"
                                   "x:b2,y:b3,");
        bmds::RawPad raw;
        raw.hats[0] = bmds::RawPad::HatUp | bmds::RawPad::HatLeft;
        raw.buttons = 1u << 6; // L2, as a button
        raw.axes[0] = 2000;    // in the dead zone
        raw.axes[1] = -32768;
        bmds::PadState s;
        m.apply(raw, s);
        CHECK(s.isDown(bmds::PadState::Up));
        CHECK(s.isDown(bmds::PadState::Left));
        CHECK(!s.isDown(bmds::PadState::Down));
        CHECK(s.isDown(bmds::PadState::L2));
        CHECK(s.axes[bmds::PadState::LeftTrigger] == 32767);
        CHECK(s.axes[bmds::PadState::LeftX] == 0);
        CHECK(s.axes[bmds::PadState::LeftY] == -32767);
}

TEST_CASE("PadMapping: parsed, formatted back, found in a database, applied to DragonRise's pad") {
        const char      *line = "a:b2,b:b1,back:b8,dpdown:+a4,dpleft:-a3,dpright:+a3,dpup:-a4,lefttrigger:a2,"
                                "leftx:a0~,start:b9,";
        bmds::PadMapping m;
        REQUIRE(bmds::PadMapping::parse(line, m).isOk());
        char text[bmds::PadMapping::MaxText];
        m.format(text, sizeof(text));
        CHECK(std::string(text) == line);
        CHECK(bmds::PadMapping::parse("a:b99", m) == bmds::Error::Invalid);
        CHECK(bmds::PadMapping::parse("a:h0.0", m) == bmds::Error::Invalid);
        CHECK(bmds::PadMapping::parse("a:x1", m) == bmds::Error::Invalid);
        CHECK(bmds::PadMapping::parse("misc1:b3,platform:BMDS,crc:1234", m).isOk()); // unknown: skipped

        const char      *database = "# a comment\n"
                                    "03000000790000001100000000000000,Linux's,a:b0,platform:Linux,\n"
                                    "030000007900000012000000000000,Short,a:b0,\n"
                                    "03000000790000001100000000000000,DragonRise Retro Pad,a:b2,b:b1,dpleft:-a3,"
                                    "dpup:-a4,start:b9,platform:BMDS,\n";
        bmds::StringView name;
        REQUIRE(bmds::PadMapping::find(database, 0x0079, 0x0011, m, &name));
        CHECK(name == bmds::StringView("DragonRise Retro Pad"));
        CHECK(!bmds::PadMapping::find(database, 0x0079, 0x0012, m, nullptr));

        const bmds::PadLayout l = padOf(DragonRise, sizeof(DragonRise));
        uint8_t               r[8];
        dragonRiseReport(r, 0x00, 0x00, 1u << 2); // left and up, B
        bmds::RawPad   raw;
        bmds::PadState s;
        REQUIRE(l.decode(Span<const uint8_t>(r, sizeof(r)), raw));
        m.apply(raw, s);
        CHECK(s.buttons == (1u << bmds::PadState::South | 1u << bmds::PadState::Left | 1u << bmds::PadState::Up));
        dragonRiseReport(r, 0x7f, 0x7f, 1u << 9);
        REQUIRE(l.decode(Span<const uint8_t>(r, sizeof(r)), raw));
        m.apply(raw, s);
        CHECK(s.buttons == 1u << bmds::PadState::Start);
}

TEST_CASE("Chord: parsed, formatted, matched") {
        bmds::Chord c;
        char        text[bmds::Chord::MaxText];
        REQUIRE(bmds::Chord::parse("ctrl+shift+t", c).isOk());
        CHECK(c.usage == 0x00070017u);
        CHECK(c.modifiers == (bmds::Chord::Ctrl | bmds::Chord::Shift));
        c.format(text, sizeof(text));
        CHECK(std::string(text) == "Ctrl+Shift+T");
        CHECK(c.matches(0x00070017u, bmds::Chord::Ctrl | bmds::Chord::Shift | bmds::Chord::Alt));
        CHECK(!c.matches(0x00070017u, bmds::Chord::Ctrl));
        const struct {
                        const char *in, *out;
        } round[] = {{"F5", "F5"},
                     {"f24", "F24"},
                     {"Esc", "Escape"},
                     {"0", "0"},
                     {"7", "7"},
                     {"KeypadEnter", "KeypadEnter"},
                     {"Alt+F4", "Alt+F4"},
                     {"VolUp", "VolUp"},
                     {"0x000C0123", "0x000C0123"},
                     {"Win", "Win"},
                     {"rightctrl", "RightCtrl"}};
        for (const auto &r : round) {
                INFO(r.in);
                REQUIRE(bmds::Chord::parse(r.in, c).isOk());
                c.format(text, sizeof(text));
                CHECK(std::string(text) == r.out);
        }
        // A side-less modifier key: either side, its own bit aside.
        REQUIRE(bmds::Chord::parse("Ctrl", c).isOk());
        CHECK(c.matches(0x000700e0u, bmds::Chord::Ctrl));
        CHECK(c.matches(0x000700e4u, bmds::Chord::Ctrl));
        CHECK(!c.matches(0x000700e1u, bmds::Chord::Shift));
        REQUIRE(bmds::Chord::parse("LeftCtrl", c).isOk());
        CHECK(!c.matches(0x000700e4u, bmds::Chord::Ctrl));
        for (const char *bad : {"", "Ctrl+", "Hyper+A", "F25", "Foo", "0x12"}) {
                INFO(bad);
                CHECK(bmds::Chord::parse(bad, c) == bmds::Error::Invalid);
        }
        CHECK(bmds::Chord::modifiersOf(0x11) == bmds::Chord::Ctrl);
        CHECK(bmds::Chord::modifiersOf(0x84) == (bmds::Chord::Alt | bmds::Chord::Win));
}

namespace {

        constexpr const char *GameControls = "[Controls]\n"
                                             "Sets=play;menu\n"
                                             "\n"
                                             "[Action turn-left]\n"
                                             "Id=3\n"
                                             "Label=Turn left\n"
                                             "Group=Movement\n"
                                             "Sets=play\n"
                                             "Keys=Z;Ctrl\n"
                                             "Pad=Pad.South;Pad.LeftX-\n"
                                             "\n"
                                             "[Action undo]\n"
                                             "Id=4\n"
                                             "Sets=play\n"
                                             "Keys=Ctrl+Z\n"
                                             "\n"
                                             "[Action steer]\n"
                                             "Id=5\n"
                                             "Kind=axis\n"
                                             "Keys=Left Right;A D\n"
                                             "Pad=Pad.LeftX;Pad.Left Pad.Right\n"
                                             "\n"
                                             "[Action down]\n"
                                             "Id=6\n"
                                             "Sets=menu\n"
                                             "Keys=Down\n"
                                             "Repeat=true\n";

        struct Told {
                        std::vector<bmds::Controls::Output> outputs;

                        static void emit(void *arg, const bmds::Controls::Output &o) {
                                static_cast<Told *>(arg)->outputs.push_back(o);
                                return;
                        }

                        std::string take() {
                                std::string s;
                                for (const auto &o : outputs) {
                                        static const char *const Types[] = {"", "press", "release", "repeat", "axis"};
                                        s += std::to_string(o.id) + " " + Types[int(o.type)] + " " +
                                             std::to_string(o.value) + ";";
                                }
                                outputs.clear();
                                return s;
                        }
        };

        constexpr uint32_t K(uint8_t usage) {
                return 0x00070000u | usage;
        }

        bmds::PadEvent padButton(uint8_t pad, uint8_t button, int32_t down) {
                return bmds::PadEvent{bmds::PadEvent::Type::Button, pad, button, down};
        }

        bmds::PadEvent padAxis(uint8_t pad, uint8_t axis, int32_t value) {
                return bmds::PadEvent{bmds::PadEvent::Type::Axis, pad, axis, value};
        }

} // namespace

TEST_CASE("Controls: a declaration read; keys and pads matched in the active set") {
        static bmds::Controls c; // big: not on the stack
        uint32_t              line = 0;
        REQUIRE(c.declare(GameControls, line).isOk());
        CHECK(c.count() == 4);
        CHECK(std::string(c.action(0).label) == "Turn left");
        CHECK(std::string(c.action(2).label) == "steer"); // its name without a label
        using A = bmds::KeyEvent::Action;
        Told t;
        // Z presses turn-left; Ctrl+Z is undo, not turn-left (more modifiers win).
        CHECK(c.key(K(0x1d), 0, A::Press, Told::emit, &t));
        CHECK(t.take() == "3 press 1;");
        CHECK(c.key(K(0x1d), 0, A::Repeat, Told::emit, &t));
        CHECK(t.take().empty()); // no Repeat=true
        CHECK(c.key(K(0x1d), 0, A::Release, Told::emit, &t));
        CHECK(t.take() == "3 release 0;");
        CHECK(c.key(K(0x1d), bmds::Chord::Ctrl, A::Press, Told::emit, &t));
        CHECK(t.take() == "4 press 1;");
        CHECK(c.key(K(0x1d), bmds::Chord::Ctrl, A::Release, Told::emit, &t));
        CHECK(t.take() == "4 release 0;");
        // Two sources of one button: down while either is.
        CHECK(c.key(K(0xe4), bmds::Chord::Ctrl, A::Press, Told::emit, &t)); // right Ctrl
        CHECK(c.pad(padButton(0, bmds::PadState::South, 1), Told::emit, &t));
        CHECK(c.key(K(0xe4), bmds::Chord::Ctrl, A::Release, Told::emit, &t));
        CHECK(c.pad(padButton(0, bmds::PadState::South, 0), Told::emit, &t));
        CHECK(t.take() == "3 press 1;3 release 0;");
        // An unbound key and an unbound button stay raw.
        CHECK(!c.key(K(0x04 + 'q' - 'a'), 0, A::Press, Told::emit, &t));
        CHECK(!c.pad(padButton(0, bmds::PadState::Start, 1), Told::emit, &t));
        // The axis: keys as -1 and 1, both 0; the stick's value as it moves; its half as a button.
        CHECK(c.key(K(0x50), 0, A::Press, Told::emit, &t)); // Left
        CHECK(c.key(K(0x07), 0, A::Press, Told::emit, &t)); // D
        CHECK(c.key(K(0x50), 0, A::Release, Told::emit, &t));
        CHECK(c.key(K(0x07), 0, A::Release, Told::emit, &t));
        CHECK(t.take() == "5 axis -32767;5 axis 0;5 axis 32767;5 axis 0;");
        CHECK(c.pad(padAxis(1, bmds::PadState::LeftX, -20000), Told::emit, &t));
        CHECK(t.take() == "3 press 1;5 axis -20000;"); // in declaration order
        CHECK(c.pad(padAxis(1, bmds::PadState::LeftX, -10000), Told::emit, &t));
        CHECK(t.take() == "3 release 0;5 axis -10000;");
        CHECK(c.pad(padAxis(1, bmds::PadState::LeftX, 0), Told::emit, &t));
        CHECK(t.take() == "5 axis 0;");
        CHECK(c.pad(padButton(0, bmds::PadState::Right, 1), Told::emit, &t));
        CHECK(t.take() == "5 axis 32767;");
        // Another set: what was held let go; play's bindings raw now, menu's matched, repeating.
        REQUIRE(c.setSet("menu", Told::emit, &t).isOk());
        CHECK(t.take() == "5 axis 0;");
        CHECK(!c.key(K(0x1d), 0, A::Press, Told::emit, &t));
        CHECK(c.key(K(0x51), 0, A::Press, Told::emit, &t));
        CHECK(c.key(K(0x51), 0, A::Repeat, Told::emit, &t));
        CHECK(t.take() == "6 press 1;6 repeat 1;");
        CHECK(c.setSet("nope", Told::emit, &t) == bmds::Error::NotExist);
        // No set: everything raw.
        REQUIRE(c.setSet("", Told::emit, &t).isOk());
        CHECK(t.take() == "6 release 0;");
        CHECK(!c.key(K(0x51), 0, A::Press, Told::emit, &t));

        char   text[64];
        size_t n = 0;
        REQUIRE(c.bindingText(5, false, text, sizeof(text), n).isOk());
        CHECK(std::string(text) == "Left Right; A D");
        REQUIRE(c.bindingText(5, true, text, sizeof(text), n).isOk());
        CHECK(std::string(text) == "Pad.LeftX; Pad.Left Pad.Right");
        REQUIRE(c.bindingText(3, true, text, sizeof(text), n).isOk());
        CHECK(std::string(text) == "Pad.South; Pad.LeftX-");
        CHECK(c.bindingText(99, false, text, sizeof(text), n) == bmds::Error::NotExist);

        // The user's bindings laid over: Keys and Pad replaced; unknown actions and bad lines skipped.
        c.applyUser("[Action turn-left]\nKeys=Q\nPad=\n[Action nope]\nKeys=W\n[Action undo]\nKeys=Hyper+U\n");
        REQUIRE(c.bindingText(3, false, text, sizeof(text), n).isOk());
        CHECK(std::string(text) == "Q");
        REQUIRE(c.bindingText(3, true, text, sizeof(text), n).isOk());
        CHECK(std::string(text).empty());
        REQUIRE(c.bindingText(4, false, text, sizeof(text), n).isOk());
        CHECK(std::string(text) == "Ctrl+Z");
}

TEST_CASE("Controls: declarations refused") {
        static bmds::Controls c;
        uint32_t              line = 0;
        const struct {
                        const char *text;
                        uint32_t    line;
        } bad[] = {
                {"[Action a]\nId=1\n", 0},                                   // no keys: the keyboard is first-class
                {"[Action a]\nKeys=A\n", 0},                                 // no Id
                {"[Action a]\nId=1\nKeys=A\n[Action b]\nId=1\nKeys=B\n", 5}, // an Id twice
                {"[Action a]\nId=1\nKeys=A\n[Action a]\nId=2\nKeys=B\n", 4}, // a name twice
                {"[Action A]\nId=1\nKeys=A\n", 1},                           // a name's characters
                {"[Action a]\nId=70000\nKeys=A\n", 2},
                {"[Action a]\nId=1\nKeys=Hyper+A\n", 3},
                {"[Action a]\nId=1\nKind=lever\nKeys=A\n", 3},
                {"[Action a]\nId=1\nKind=axis\nKeys=A\n", 4},     // an axis's keys are pairs
                {"[Action a]\nId=1\nKeys=A\nPad=Pad.LeftX\n", 4}, // a button's pad is never an axis
                {"[Action a]\nId=1\nSets=play\nKeys=A\n", 3},     // sets are declared first
                {"[Action a]\nId=1\nKeys=A\nthis is not ini\n", 4},
                {"", 0},
        };
        for (const auto &b : bad) {
                INFO(b.text);
                CHECK(c.declare(b.text, line).isError());
                CHECK(line == b.line);
                CHECK(!c.isDeclared());
        }
        // Forward compatible: unknown keys and groups skipped.
        CHECK(c.declare("[Controls]\nFuture=1\n[Other]\nX=1\n[Action a]\nId=1\nKeys=A\nTooltip=x\n", line).isOk());
}

TEST_CASE("Controls: bindings changed, conflicts found, the user's file as the difference") {
        static bmds::Controls defaults, c;
        uint32_t              line = 0;
        REQUIRE(defaults.declare(GameControls, line).isOk());
        REQUIRE(c.declare(GameControls, line).isOk());
        char text[512];
        CHECK(bmds::Controls::formatUser(defaults, c, text, sizeof(text)) == 0);
        const int turn = c.indexOf("turn-left"), undo = c.indexOf("undo"), steer = c.indexOf("steer"),
                  down = c.indexOf("down");
        REQUIRE(turn >= 0);
        REQUIRE(c.indexOf("nope") == -1);
        // Ctrl+Z is undo's: turn-left given it conflicts with undo (both in play), not with down (menu).
        REQUIRE(c.setBinding(size_t(turn), false, "Ctrl+Z; Q").isOk());
        CHECK(c.conflicts(size_t(turn), false) == uint64_t(1) << undo);
        // Down is menu's down's key: no conflict, the sets differ. Steer is in every set: it does.
        REQUIRE(c.setBinding(size_t(turn), false, "Down").isOk());
        CHECK(c.conflicts(size_t(turn), false) == 0);
        REQUIRE(c.setBinding(size_t(turn), false, "Left").isOk());
        CHECK(c.conflicts(size_t(turn), false) == uint64_t(1) << steer);
        // A side-less Ctrl meets a right Ctrl.
        REQUIRE(c.setBinding(size_t(undo), false, "RightCtrl").isOk());
        REQUIRE(c.setBinding(size_t(turn), false, "Ctrl").isOk());
        CHECK(c.conflicts(size_t(turn), false) == uint64_t(1) << undo);
        // Pads: South against a pair's half.
        REQUIRE(c.setBinding(size_t(steer), true, "Pad.South Pad.East").isOk());
        CHECK(c.conflicts(size_t(turn), true) == uint64_t(1) << steer);
        // Refused: no key left, nonsense, a button's pad as an axis.
        CHECK(c.setBinding(size_t(turn), false, "") == bmds::Error::Invalid);
        CHECK(c.setBinding(size_t(turn), false, "Hyper+A") == bmds::Error::Invalid);
        CHECK(c.setBinding(size_t(turn), true, "Pad.LeftX") == bmds::Error::Invalid);
        REQUIRE(c.setBinding(size_t(turn), true, "").isOk()); // no pad is allowed
        (void)down;
        const size_t n = bmds::Controls::formatUser(defaults, c, text, sizeof(text));
        CHECK(std::string(text, n) == "[Action turn-left]\nKeys=Ctrl\nPad=\n"
                                      "\n[Action undo]\nKeys=RightCtrl\n"
                                      "\n[Action steer]\nPad=Pad.South Pad.East\n");
        // Read back over the defaults: the same bindings.
        static bmds::Controls again;
        REQUIRE(again.declare(GameControls, line).isOk());
        again.applyUser(bmds::StringView(text, n));
        char   a[64], b[64];
        size_t la = 0, lb = 0;
        for (size_t i = 0; i < c.count(); ++i)
                for (int pad = 0; pad < 2; ++pad) {
                        REQUIRE(c.bindingText(c.action(i).id, pad == 1, a, sizeof(a), la).isOk());
                        REQUIRE(again.bindingText(again.action(i).id, pad == 1, b, sizeof(b), lb).isOk());
                        CHECK(std::string(a) == std::string(b));
                }
}
