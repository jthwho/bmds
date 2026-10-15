/**
 * @file      hiddescriptor.h
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
 * @brief A HID report descriptor's input elements, for the layouts built on
 *        it (`MouseLayout`, `PadLayout`; `devplan/controls.md` §7a).
 *
 * `parse()` walks the descriptor's items and hands each element of each
 * Input main item to a visitor: nothing is stored, so each user keeps only
 * what it needs. `extract()` reads an element's value from a report.
 *
 * Usages are extended: the page in the high half, as a 4-byte usage item
 * gives them.
 *
 * @par Thread Safety
 * Pure functions.
 */
namespace HidDescriptor {

        /** @brief An Input item's flags (its data's low bits). */
        enum Flag : uint8_t {
                Constant = 0x01,  ///< Padding, or a constant: never visited.
                Variable = 0x02,  ///< A value per usage; else an array of usage indices.
                Relative = 0x04,  ///< A change since the last report; else absolute.
                NullState = 0x40, ///< Values outside the logical range mean "none".
        };

        /** @brief One element of an Input item. */
        struct Element {
                        uint32_t application = 0; ///< The usage of the application collection it is in (0: none).
                        uint32_t usage = 0;       ///< Its usage; an array's first.
                        uint32_t usageMax = 0;    ///< An array's last usage; a variable's own.
                        int32_t  logicalMin = 0;
                        int32_t  logicalMax = 0; ///< Unsigned in the item when the minimum isn't negative.
                        uint16_t offset = 0;     ///< In bits, after the report ID.
                        uint8_t  size = 0;       ///< In bits, 1 to 32.
                        uint8_t  reportId = 0;   ///< 0: the device numbers no reports.
                        uint8_t  collection = 0; ///< Application collections opened before its own (0 up).
                        uint8_t  flags = 0;      ///< `Flag` bits.

                        /** @brief Returns true if its values are signed (a negative minimum). */
                        bool isSigned() const { return logicalMin < 0; }
        };

        /** @brief Told of each element, in the descriptor's order. */
        using Visitor = void (*)(void *arg, const Element &element);

        /**
         * @brief Hands each element of each non-constant Input item of @p descriptor to
         *        @p visit. A malformed or truncated descriptor ends the walk where it breaks.
         */
        void parse(Span<const uint8_t> descriptor, Visitor visit, void *arg);

        /**
         * @brief Reads @p size bits at bit @p offset of @p data (a report after its ID),
         *        sign-extended when @p isSigned.
         * @return False if the field runs past the end.
         * @hotpath
         */
        bool extract(Span<const uint8_t> data, uint16_t offset, uint8_t size, bool isSigned, int32_t &out);

} // namespace HidDescriptor

BMDS_NAMESPACE_END
