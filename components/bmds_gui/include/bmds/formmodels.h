/**
 * @file      formmodels.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Scrolling by whole rows (`devplan/gui.md` §4u: a `ScrollView`), pure.
 *
 * A row's stop is the offset that puts it at the top of the view; the stops
 * rise and the first is 0.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace RowScroll {

        /**
         * @brief Returns the first row that, at the top, shows the rest of the
         *        content (@p total high) in @p page: the last row worth scrolling to.
         */
        size_t lastTop(Span<const uint32_t> stops, uint32_t total, uint32_t page);

        /**
         * @brief Returns the row, at most @p most, whose stop is nearest @p value
         *        (a scroll bar's), but at least one row from @p current toward
         *        @p value when @p value isn't @p current's stop: a small step moves.
         */
        size_t snap(Span<const uint32_t> stops, uint32_t value, size_t current, size_t most);

        /**
         * @brief Returns the top row that shows row @p row (content from @p top to
         *        @p bottom) with as little scrolling from @p current as there can be;
         *        @p row itself if it is taller than @p page.
         */
        size_t reveal(Span<const uint32_t> stops, uint32_t top, uint32_t bottom, size_t row, size_t current,
                      uint32_t page);

} // namespace RowScroll

/**
 * @brief What a number field holds (`devplan/gui.md` §4u): a value kept in a
 *        signed range, its steps, and the text typed for it.
 *
 * @par Thread Safety
 * A plain value.
 */
class NumberModel {
        public:
                /** @brief Sets the range (swapped if backwards), the value kept in it. */
                void setRange(int32_t minimum, int32_t maximum);

                /** @brief Returns the smallest value. */
                int32_t minimum() const { return _min; }

                /** @brief Returns the largest value. */
                int32_t maximum() const { return _max; }

                /** @brief Sets an arrow's step and a page's (at least 1 each). */
                void setSteps(uint32_t step, uint32_t page);

                /** @brief Returns the value. */
                int32_t value() const { return _value; }

                /** @brief Sets the value, kept in range. @return True if it changed. */
                bool setValue(int64_t value);

                /** @brief Moves @p count steps (pages if @p page), kept in range. @return True if it changed. */
                bool step(int64_t count, bool page);

                /**
                 * @brief Reads @p text as a number: decimal, a sign allowed, spaces
                 *        around; more digits than fit are kept at the extreme.
                 * @return The number, or `Invalid`.
                 */
                static Result<int64_t> parse(StringView text);

        private:
                int32_t  _min = 0;
                int32_t  _max = 100;
                int32_t  _value = 0;
                uint32_t _step = 1;
                uint32_t _page = 10;
};

/**
 * @brief Colors as hue, saturation and value, and as text (`devplan/gui.md`
 *        §4u: the color dialog), pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace ColorModel {

        /** @brief A color by hue (0-359 degrees), saturation and value (0-255). */
        struct Hsv {
                        uint16_t hue = 0;
                        uint8_t  saturation = 0;
                        uint8_t  value = 0;
        };

        /** @brief Returns @p hsv as 0xRRGGBB. */
        uint32_t toRgb(const Hsv &hsv);

        /** @brief Returns 0xRRGGBB @p rgb as hue, saturation and value (a grey's hue 0). */
        Hsv fromRgb(uint32_t rgb);

        /**
         * @brief Reads `rrggbb` or `#rrggbb` (either case, spaces around).
         * @return 0xRRGGBB, or `Invalid`.
         */
        Result<uint32_t> parseHex(StringView text);

        /** @brief Writes @p rgb as `#rrggbb` into @p out. */
        void formatHex(uint32_t rgb, char (&out)[8]);

} // namespace ColorModel

/**
 * @brief A list kept as text, its items between separators (`devplan/gui.md`
 *        §4u: the list editor; a `paths` setting's `;`), pure.
 *
 * The empty text is no items; otherwise there is one more item than
 * separators, and an item may be empty. An item put in that holds the
 * separator becomes several. @p out must not hold @p text.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace ListText {

        /** @brief Returns the items in @p text. */
        size_t count(StringView text, char separator);

        /** @brief Returns item @p index (empty past the end). */
        StringView item(StringView text, char separator, size_t index);

        /**
         * @brief Sets @p out to @p text with @p item put in before item @p index
         *        (at the end past it). @return `Ok` or `NoMem`.
         */
        Error insert(StringView text, char separator, size_t index, StringView item, String &out);

        /** @brief Sets @p out to @p text with item @p index replaced. @return `Ok`, `Invalid` (none), `NoMem`. */
        Error replace(StringView text, char separator, size_t index, StringView item, String &out);

        /** @brief Sets @p out to @p text without item @p index. @return `Ok`, `Invalid` (none), `NoMem`. */
        Error remove(StringView text, char separator, size_t index, String &out);

        /**
         * @brief Sets @p out to @p text with item @p from moved to place @p to
         *        (counted without it). @return `Ok`, `Invalid` (no such item or place), `NoMem`.
         */
        Error move(StringView text, char separator, size_t from, size_t to, String &out);

} // namespace ListText

BMDS_NAMESPACE_END
