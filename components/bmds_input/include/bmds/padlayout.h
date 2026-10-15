/**
 * @file      padlayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A pad's controls as its device numbers them: axes, hats and
 *        buttons (`devplan/controls.md` §7a). `PadMapping` makes the
 *        standard pad of it.
 *
 * @par Thread Safety
 * A plain value.
 */
struct RawPad {
                static constexpr size_t MaxAxes = 8;
                static constexpr size_t MaxHats = 2;
                static constexpr size_t MaxButtons = 32;

                /** @brief A hat's direction bits (SDL's): none down is centred. */
                enum Hat : uint8_t {
                        HatUp = 0x01,
                        HatRight = 0x02,
                        HatDown = 0x04,
                        HatLeft = 0x08,
                };

                int16_t  axes[MaxAxes] = {}; ///< −32768 (its logical minimum) to 32767 (its maximum).
                uint8_t  hats[MaxHats] = {}; ///< `Hat` bits.
                uint32_t buttons = 0;        ///< Bit N: button N + 1 (`bN` in a mapping) down.
};

/**
 * @brief Where a HID joystick or game pad puts its axes, hats and buttons,
 *        from its report descriptor.
 *
 * `parse()` takes the first Joystick, Game Pad or Multi-axis Controller
 * collection's absolute inputs: Generic Desktop X to Wheel as axes, in the
 * descriptor's order (pads repeat usages: numbering by position is what
 * stays unambiguous), Hat Switch as hats, the Button page by number.
 * `decode()` turns one of its reports into a `RawPad`.
 *
 * @par Thread Safety
 * A plain value.
 */
class PadLayout {
        public:
                /** @brief One value of a report. */
                struct Field {
                                int32_t  min = 0;
                                int32_t  max = 0;
                                uint16_t offset = 0; ///< In bits, after the report ID.
                                uint8_t  size = 0;   ///< In bits (0: none).
                                bool     isSigned = false;
                };

                /**
                 * @brief Reads @p descriptor (a HID report descriptor).
                 * @return The layout; `isValid()` false when it has no joystick or game pad.
                 */
                static PadLayout parse(Span<const uint8_t> descriptor);

                /** @brief Returns true if a joystick or game pad with a control was found. */
                bool isValid() const { return _axisCount + _hatCount + _buttonCount != 0; }

                /** @brief Returns the reports' ID (0: the device numbers none). */
                uint8_t reportId() const { return _reportId; }

                /** @brief Returns the axes' count. */
                uint8_t axisCount() const { return _axisCount; }

                /** @brief Returns the hats' count. */
                uint8_t hatCount() const { return _hatCount; }

                /** @brief Returns the buttons' count: the highest button's number. */
                uint8_t buttonCount() const { return _buttonCount; }

                /** @brief Returns axis @p index's usage (Generic Desktop 0x30 to 0x38), 0 past the last. */
                uint8_t axisUsage(size_t index) const { return index < _axisCount ? _axisUsages[index] : 0; }

                /**
                 * @brief Turns @p report (as received: its ID first when the device numbers
                 *        them) into @p pad.
                 * @return False if it isn't this pad's report (another ID, or too short).
                 * @hotpath
                 */
                bool decode(Span<const uint8_t> report, RawPad &pad) const;

        private:
                struct Builder;

                Field    _axes[RawPad::MaxAxes];
                Field    _hats[RawPad::MaxHats];
                uint16_t _buttons[RawPad::MaxButtons] = {}; // bit offsets
                uint8_t  _axisUsages[RawPad::MaxAxes] = {};
                uint8_t  _axisCount = 0;
                uint8_t  _hatCount = 0;
                uint8_t  _buttonCount = 0;
                uint8_t  _reportId = 0;
                uint32_t _buttonMask = 0; // the buttons it has
};

BMDS_NAMESPACE_END
