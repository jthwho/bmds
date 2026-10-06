/**
 * @file      mouselayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/inputevent.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where a report-protocol mouse puts its buttons, motion and wheel,
 *        from its HID report descriptor (`devplan/input.md` §5a).
 *
 * `parse()` reads the descriptor's items and takes the first application
 * collection of a mouse (Generic Desktop, Mouse): in its input reports, the
 * buttons (the Button page, up to 8), X, Y and the wheel. `decode()` turns
 * one of its reports into a `PointerEvent`.
 *
 * @par Thread Safety
 * A plain value.
 */
class MouseLayout {
        public:
                /** @brief One field of a report: where its bits are. */
                struct Field {
                                uint16_t offset = 0; ///< In bits, after the report ID.
                                uint8_t  size = 0;   ///< In bits (0: none); up to 32.
                                bool     isSigned = false;
                };

                /**
                 * @brief Reads @p descriptor (a HID report descriptor).
                 * @return The layout; `isValid()` false when it has no mouse
                 *         with X and Y.
                 */
                static MouseLayout parse(Span<const uint8_t> descriptor);

                /** @brief Returns true if a mouse with X and Y was found. */
                bool isValid() const { return _x.size != 0 && _y.size != 0; }

                /** @brief Returns the reports' ID (0: the device numbers none). */
                uint8_t reportId() const { return _reportId; }

                /** @brief Returns true if it has a wheel. */
                bool hasWheel() const { return _wheel.size != 0; }

                /** @brief Returns the buttons' count (up to 8). */
                uint8_t buttonCount() const { return _buttonCount; }

                /**
                 * @brief Turns @p report (as received: its ID first when the
                 *        device numbers them) into @p event, motion and wheel
                 *        clamped to the event's ranges.
                 * @return False if it isn't this mouse's report (another ID, or
                 *         too short).
                 * @hotpath
                 */
                bool decode(Span<const uint8_t> report, PointerEvent &event) const;

        private:
                Field    _x;
                Field    _y;
                Field    _wheel;
                uint16_t _buttons = 0; // the first button's bit offset
                uint8_t  _buttonCount = 0;
                uint8_t  _reportId = 0;
};

BMDS_NAMESPACE_END
