/**
 * @file      deadline.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/config.h>
#include <bmds/detail/ticks.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One timeout shared across several waits.
 *
 * A blocking call that may wait more than once (for a lock, then for
 * space, then for space again) creates a `Deadline` from its `timeoutMs`
 * and passes it to each wait, so the total never exceeds the caller's
 * timeout. `WaitForever` never expires.
 *
 * @par Thread Safety
 * Not thread-safe; a local object of one call.
 */
class Deadline {
        public:
                /** @brief Starts the clock. */
                explicit Deadline(unsigned int timeoutMs) : _ticks(Detail::toTicks(timeoutMs)) {
                        vTaskSetTimeOutState(&_timeOut);
                }

                Deadline(const Deadline &) = delete;
                Deadline &operator=(const Deadline &) = delete;

                /** @brief Returns the ticks left: 0 once expired, `portMAX_DELAY` for `WaitForever`. */
                TickType_t remainingTicks() {
                        if (_ticks == portMAX_DELAY) return portMAX_DELAY;
                        if (xTaskCheckForTimeOut(&_timeOut, &_ticks) == pdTRUE) _ticks = 0;
                        return _ticks;
                }

                /** @brief Returns true once the timeout has passed. */
                bool isExpired() { return remainingTicks() == 0; }

        private:
                TimeOut_t  _timeOut;
                TickType_t _ticks;
};

BMDS_NAMESPACE_END
