/**
 * @file      inputevent.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Keys by HID usage (the keyboard page, 0x07): the ones BMDS names.
 *
 * Letters run from `A` to `Z`, digits from `Digit1` to `Digit0`, function
 * keys from `F1` to `F12`, the keypad's digits from `Keypad1` to `Keypad0`.
 */
namespace HidKey {

        constexpr uint8_t None = 0x00;
        constexpr uint8_t ErrorRollOver = 0x01; ///< Too many keys down: the report means nothing.
        constexpr uint8_t A = 0x04;
        constexpr uint8_t Z = 0x1d;
        constexpr uint8_t Digit1 = 0x1e;
        constexpr uint8_t Digit0 = 0x27;
        constexpr uint8_t Enter = 0x28;
        constexpr uint8_t Escape = 0x29;
        constexpr uint8_t Backspace = 0x2a;
        constexpr uint8_t Tab = 0x2b;
        constexpr uint8_t Space = 0x2c;
        constexpr uint8_t CapsLock = 0x39;
        constexpr uint8_t F1 = 0x3a;
        constexpr uint8_t F12 = 0x45;
        constexpr uint8_t PrintScreen = 0x46;
        constexpr uint8_t ScrollLock = 0x47;
        constexpr uint8_t Pause = 0x48;
        constexpr uint8_t Insert = 0x49;
        constexpr uint8_t Home = 0x4a;
        constexpr uint8_t PageUp = 0x4b;
        constexpr uint8_t Delete = 0x4c;
        constexpr uint8_t End = 0x4d;
        constexpr uint8_t PageDown = 0x4e;
        constexpr uint8_t Right = 0x4f;
        constexpr uint8_t Left = 0x50;
        constexpr uint8_t Down = 0x51;
        constexpr uint8_t Up = 0x52;
        constexpr uint8_t NumLock = 0x53;
        constexpr uint8_t KeypadSlash = 0x54;
        constexpr uint8_t KeypadEnter = 0x58;
        constexpr uint8_t Keypad1 = 0x59;
        constexpr uint8_t Keypad0 = 0x62;
        constexpr uint8_t KeypadPeriod = 0x63;
        constexpr uint8_t NonUsBackslash = 0x64;
        constexpr uint8_t Application = 0x65;

} // namespace HidKey

/**
 * @brief A key pressed, held (repeating) or released.
 *
 * @par Thread Safety
 * A plain value.
 */
struct KeyEvent {
                /** @brief What happened to the key. */
                enum class Action : uint8_t {
                        Press,   ///< Went down.
                        Repeat,  ///< Still down: the host's key repeat.
                        Release, ///< Came up.
                };

                /** @brief Modifier bits: a boot keyboard report's first byte. */
                enum Modifier : uint8_t {
                        LeftCtrl = 0x01,
                        LeftShift = 0x02,
                        LeftAlt = 0x04,
                        LeftGui = 0x08,
                        RightCtrl = 0x10,
                        RightShift = 0x20,
                        RightAlt = 0x40,
                        RightGui = 0x80,
                        Ctrl = LeftCtrl | RightCtrl,
                        Shift = LeftShift | RightShift,
                        Alt = LeftAlt | RightAlt,
                        Gui = LeftGui | RightGui,
                };

                /** @brief Lock bits: a boot keyboard's LED report. */
                enum Lock : uint8_t {
                        NumLock = 0x01,
                        CapsLock = 0x02,
                        ScrollLock = 0x04,
                };

                uint8_t  usage = HidKey::None; ///< The key (`HidKey`).
                uint8_t  modifiers = 0;        ///< `Modifier` bits down with it.
                uint8_t  locks = 0;            ///< `Lock` bits on.
                Action   action = Action::Press;
                uint32_t codepoint = 0; ///< The character the keymap gives it (shift and locks applied); 0: none.

                /** @brief Returns true if a Ctrl key is down. */
                constexpr bool hasCtrl() const { return (modifiers & Ctrl) != 0; }

                /** @brief Returns true if a Shift key is down. */
                constexpr bool hasShift() const { return (modifiers & Shift) != 0; }

                /** @brief Returns true if an Alt key is down. */
                constexpr bool hasAlt() const { return (modifiers & Alt) != 0; }
};

/**
 * @brief A mouse's motion and buttons since its last report.
 *
 * @par Thread Safety
 * A plain value.
 */
struct PointerEvent {
                /** @brief Button bits. */
                enum Button : uint8_t {
                        Left = 0x01,
                        Right = 0x02,
                        Middle = 0x04,
                };

                int16_t dx = 0;      ///< Right is positive.
                int16_t dy = 0;      ///< Down is positive.
                int8_t  wheel = 0;   ///< Away from the user is positive.
                uint8_t buttons = 0; ///< `Button` bits down now.
};

/**
 * @brief A gamepad's standard control changed, or a pad came or went (`devplan/controls.md`
 *        §7b): what `BMDS_EVENT_PAD` carries.
 *
 * @par Thread Safety
 * A plain value.
 */
struct PadEvent {
                /** @brief What happened: `BMDS_PAD_BUTTON`'s values, in order. */
                enum class Type : uint8_t {
                        Button = 1, ///< `index`: a `PadState::Button`; `value`: 1 down, 0 up.
                        Axis,       ///< `index`: a `PadState::Axis`; `value`: its value.
                        Attached,   ///< `value`: vendor << 16 | product.
                        Detached,
                };

                Type    type = Type::Button;
                uint8_t pad = 0; ///< Its number.
                uint8_t index = 0;
                int32_t value = 0;
};

BMDS_NAMESPACE_END
