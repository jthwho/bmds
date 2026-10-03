/**
 * @file      ticks.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace Detail {

        /**
         * @brief Converts a BMDS timeout to FreeRTOS ticks.
         *
         * `WaitForever` becomes `portMAX_DELAY`. Other values round up, so a
         * non-zero timeout never becomes a zero-tick poll.
         */
        inline TickType_t toTicks(unsigned int timeoutMs) {
                if (timeoutMs == WaitForever) return portMAX_DELAY;
                const TickType_t ticks = (static_cast<uint64_t>(timeoutMs) * configTICK_RATE_HZ + 999) / 1000;
                return ticks < portMAX_DELAY ? ticks : portMAX_DELAY - 1;
        }

} // namespace Detail

BMDS_NAMESPACE_END
