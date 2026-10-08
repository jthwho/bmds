/**
 * @file      scrollmodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a scroll bar shows, pure (`devplan/filebrowser.md` §4e): a
 *        range, the page of it that shows, the value (the page's start) kept
 *        in range, and the bar's parts along its length.
 *
 * The value runs from 0 to `maximum()` (the range less the page). Along a bar
 * `length` long: an arrow at each end (`arrow` long, or half the bar if it is
 * shorter than two), the track between, and in the track the thumb, sized to
 * the page's share of the range (at least `MinThumb`) and placed by the value.
 * Without a thumb (nothing to scroll, or no room) the track is all there is
 * between the arrows.
 *
 * @par Thread Safety
 * A plain value.
 */
class ScrollModel {
        public:
                /** @brief The thumb's least length. */
                static constexpr uint16_t MinThumb = 8;

                /** @brief A part of the bar. */
                enum class Part : uint8_t {
                        None,
                        BackArrow,    ///< Up or left: a line back.
                        BackPage,     ///< The track before the thumb: a page back.
                        Thumb,        ///< Dragged.
                        ForwardPage,  ///< The track after the thumb: a page on.
                        ForwardArrow, ///< Down or right: a line on.
                };

                /** @brief Where the parts are along a bar, from its start. */
                struct Geometry {
                                uint16_t arrow = 0;       ///< Each arrow's length.
                                uint16_t trackStart = 0;  ///< After the first arrow.
                                uint16_t trackLength = 0; ///< Between the arrows.
                                uint16_t thumbStart = 0;  ///< In bar coordinates (0 length: no thumb).
                                uint16_t thumbLength = 0;
                };

                /** @brief Sets the range and the page that shows of it (the value kept in range). */
                void setRange(uint32_t total, uint32_t page);

                /** @brief Sets a line's length (an arrow's step; a page is the page less a line). */
                void setLineStep(uint32_t line) { _line = line > 0 ? line : 1; }

                /** @brief Returns the range. */
                uint32_t total() const { return _total; }

                /** @brief Returns the page. */
                uint32_t page() const { return _page; }

                /** @brief Returns a line's length. */
                uint32_t lineStep() const { return _line; }

                /** @brief Returns a page step: the page less a line, a line at least. */
                uint32_t pageStep() const { return _page > _line ? _page - _line : _line; }

                /** @brief Returns the largest value: the range less the page (0 if it all shows). */
                uint32_t maximum() const { return _total > _page ? _total - _page : 0; }

                /** @brief Returns the value. */
                uint32_t value() const { return _value; }

                /** @brief Returns true if there is anything to scroll. */
                bool isScrollable() const { return maximum() > 0; }

                /** @brief Sets the value, kept in range. @return True if it changed. */
                bool setValue(int64_t value);

                /** @brief Moves by @p lines lines (back if negative). @return True if it changed. */
                bool stepLines(int lines) { return setValue(int64_t(_value) + int64_t(lines) * _line); }

                /** @brief Moves by @p pages pages. @return True if it changed. */
                bool stepPages(int pages) { return setValue(int64_t(_value) + int64_t(pages) * pageStep()); }

                /** @brief Moves as a press on @p part does once. @return True if the value changed. */
                bool act(Part part);

                /** @brief Returns the parts along a bar @p length long with arrows @p arrow long. */
                Geometry geometry(uint16_t length, uint16_t arrow) const;

                /** @brief Returns the part at @p position along a bar of @p geometry's. */
                static Part partAt(const Geometry &geometry, uint16_t length, int position);

                /** @brief Returns the value a thumb dragged to start at @p thumbStart stands for (in range). */
                uint32_t valueForThumb(const Geometry &geometry, int thumbStart) const;

        private:
                uint32_t _total = 0;
                uint32_t _page = 0;
                uint32_t _line = 1;
                uint32_t _value = 0;
};

BMDS_NAMESPACE_END
