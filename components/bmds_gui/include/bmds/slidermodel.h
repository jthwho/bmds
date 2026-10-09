/**
 * @file      slidermodel.h
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
 * @brief What a slider shows (`devplan/gui.md` §4s): a signed range, the
 *        value kept in it, its steps, and where the handle sits along the
 *        slider's length.
 *
 * The handle travels the length less its own. An inverted slider (a vertical
 * one) has its maximum at the start (the top).
 *
 * @par Thread Safety
 * A plain value.
 */
class SliderModel {
        public:
                /** @brief Sets the range (swapped if backwards), the value kept in it. */
                void setRange(int32_t minimum, int32_t maximum);

                /** @brief Returns the least value. */
                int32_t minimum() const { return _min; }

                /** @brief Returns the largest value. */
                int32_t maximum() const { return _max; }

                /** @brief Returns the value. */
                int32_t value() const { return _value; }

                /** @brief Sets the value, kept in range. @return True if it changed. */
                bool setValue(int64_t value);

                /** @brief Sets a step (arrows, the wheel) and a page (Page Up, Page Down); at least 1. */
                void setSteps(int32_t step, int32_t page);

                /** @brief Moves by @p steps steps. @return True if it changed. */
                bool step(int steps) { return setValue(int64_t(_value) + int64_t(steps) * _step); }

                /** @brief Moves by @p pages pages. @return True if it changed. */
                bool stepPage(int pages) { return setValue(int64_t(_value) + int64_t(pages) * _page); }

                /** @brief Puts the maximum at the start of the length (a vertical slider's top). */
                void setInverted(bool inverted) { _inverted = inverted; }

                /** @brief Returns true if the maximum is at the start. */
                bool isInverted() const { return _inverted; }

                /** @brief Returns the handle's start along a slider @p length long, @p handle long. */
                uint16_t handleStart(uint16_t length, uint16_t handle) const;

                /**
                 * @brief Returns the value whose handle starts at @p start (in range,
                 *        the nearest).
                 */
                int32_t valueAt(int start, uint16_t length, uint16_t handle) const;

        private:
                int32_t _min = 0;
                int32_t _max = 100;
                int32_t _value = 0;
                int32_t _step = 1;
                int32_t _page = 10;
                bool    _inverted = false;
};

BMDS_NAMESPACE_END
