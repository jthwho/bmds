/**
 * @file      keyboarddecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Boot keyboard reports as key events, and the US keymap
 * (devplan/input.md I4, I5).
 */

#include <bmds/keyboarddecoder.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The modifiers as keys: usages 0xe0 (left Ctrl) to 0xe7 (right GUI),
        // a bit each in the report's first byte.
        constexpr uint8_t FirstModifierKey = 0xe0;

        // Usages 0x2c (Space) to 0x38 (/): unshifted, then shifted.
        constexpr char Punctuation[2][13] = {
                {' ', '-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/'},
                {' ', '_', '+', '{', '}', '|', '|', ':', '"', '~', '<', '>', '?'},
        };
        constexpr char DigitsShifted[10] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};

        bool isLockKey(uint8_t usage) {
                return usage == HidKey::CapsLock || usage == HidKey::NumLock || usage == HidKey::ScrollLock;
        }

        bool contains(const uint8_t *keys, size_t n, uint8_t usage) {
                for (size_t i = 0; i < n; ++i)
                        if (keys[i] == usage) return true;
                return false;
        }

} // namespace

uint32_t Keymap::us(uint8_t usage, uint8_t modifiers, uint8_t locks) {
        const bool shift = (modifiers & KeyEvent::Shift) != 0;
        if (usage >= HidKey::A && usage <= HidKey::Z) {
                const bool upper = shift != ((locks & KeyEvent::CapsLock) != 0);
                return static_cast<uint32_t>((upper ? 'A' : 'a') + (usage - HidKey::A));
        }
        if (usage >= HidKey::Digit1 && usage <= HidKey::Digit0) {
                const unsigned i = usage - HidKey::Digit1; // 1..9, then 0
                return static_cast<uint32_t>(shift ? DigitsShifted[i] : (i == 9 ? '0' : '1' + i));
        }
        if (usage >= HidKey::Space && usage <= 0x38)
                return static_cast<uint32_t>(Punctuation[shift][usage - HidKey::Space]);
        switch (usage) {
                case HidKey::KeypadSlash: return '/';
                case 0x55: return '*';
                case 0x56: return '-';
                case 0x57: return '+';
                case 0x67: return '='; // keypad =
                case HidKey::NonUsBackslash: return shift ? '|' : '\\';
                default: break;
        }
        // The keypad's digits and point type only with Num Lock (and no Shift);
        // otherwise they are the navigation keys printed on them.
        if (usage >= HidKey::Keypad1 && usage <= HidKey::KeypadPeriod && (locks & KeyEvent::NumLock) != 0 && !shift) {
                if (usage == HidKey::KeypadPeriod) return '.';
                if (usage == HidKey::Keypad0) return '0';
                return static_cast<uint32_t>('1' + (usage - HidKey::Keypad1));
        }
        return 0;
}

void KeyboardDecoder::send(uint8_t usage, KeyEvent::Action action, Emit emit, void *arg) const {
        KeyEvent event;
        event.usage = usage;
        event.modifiers = _modifiers;
        event.locks = _locks;
        event.action = action;
        if (action != KeyEvent::Action::Release) event.codepoint = Keymap::us(usage, _modifiers, _locks);
        emit(arg, event);
        return;
}

bool KeyboardDecoder::report(Span<const uint8_t> data, uint32_t nowMs, Emit emit, void *arg) {
        if (data.size() < 3) return false;
        const size_t n = data.size() - 2 < Keys ? data.size() - 2 : Keys;
        uint8_t      keys[Keys] = {};
        for (size_t i = 0; i < n; ++i) {
                keys[i] = data[2 + i];
                if (keys[i] == HidKey::ErrorRollOver) return false; // too many down: nothing known
        }
        const uint8_t modifiers = data[0];
        const uint8_t locks = _locks;

        // Releases, with the modifiers as they were.
        for (uint8_t bit = 0; bit < 8; ++bit)
                if ((_modifiers & (1u << bit)) != 0 && (modifiers & (1u << bit)) == 0)
                        send(static_cast<uint8_t>(FirstModifierKey + bit), KeyEvent::Action::Release, emit, arg);
        for (const uint8_t key : _keys) {
                if (key == HidKey::None || contains(keys, Keys, key)) continue;
                send(key, KeyEvent::Action::Release, emit, arg);
                if (key == _held) _held = HidKey::None;
        }

        // Presses, with the modifiers as they are now.
        const uint8_t before = _modifiers;
        _modifiers = modifiers;
        for (uint8_t bit = 0; bit < 8; ++bit)
                if ((before & (1u << bit)) == 0 && (modifiers & (1u << bit)) != 0)
                        send(static_cast<uint8_t>(FirstModifierKey + bit), KeyEvent::Action::Press, emit, arg);
        for (const uint8_t key : keys) {
                if (key == HidKey::None || contains(_keys, Keys, key)) continue;
                if (key == HidKey::NumLock) _locks ^= KeyEvent::NumLock;
                if (key == HidKey::CapsLock) _locks ^= KeyEvent::CapsLock;
                if (key == HidKey::ScrollLock) _locks ^= KeyEvent::ScrollLock;
                send(key, KeyEvent::Action::Press, emit, arg);
                if (!isLockKey(key)) {
                        _held = key;
                        _repeatAt = nowMs + RepeatDelayMs;
                }
        }
        for (size_t i = 0; i < Keys; ++i) _keys[i] = keys[i];
        return _locks != locks;
}

uint32_t KeyboardDecoder::repeat(uint32_t nowMs, Emit emit, void *arg) {
        if (_held == HidKey::None) return NoRepeat;
        if (static_cast<int32_t>(nowMs - _repeatAt) >= 0) {
                send(_held, KeyEvent::Action::Repeat, emit, arg);
                _repeatAt += RepeatIntervalMs;
                // Late (the task was busy): the next a whole interval from now, not a burst.
                if (static_cast<int32_t>(nowMs - _repeatAt) >= 0) _repeatAt = nowMs + RepeatIntervalMs;
        }
        return _repeatAt - nowMs;
}

void KeyboardDecoder::releaseAll(Emit emit, void *arg) {
        for (uint8_t &key : _keys) {
                if (key != HidKey::None) send(key, KeyEvent::Action::Release, emit, arg);
                key = HidKey::None;
        }
        for (uint8_t bit = 0; bit < 8; ++bit)
                if ((_modifiers & (1u << bit)) != 0)
                        send(static_cast<uint8_t>(FirstModifierKey + bit), KeyEvent::Action::Release, emit, arg);
        _modifiers = 0;
        _held = HidKey::None;
        return;
}

BMDS_NAMESPACE_END
