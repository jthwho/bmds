/**
 * @file      stopwatch.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Times a stretch of code (`devplan/bench.md` §4b): microseconds from
 *        the monotonic clock, and the CPU's cycles for short stretches.
 *
 * Started when made. `cycles()` is exact only on the core that started it
 * and across less than 2^32 cycles (about 11.9 s at 360 MHz): for longer
 * stretches, or a task that may move between cores, use `elapsedUs()`.
 *
 * @code
 * Stopwatch watch;
 * work();
 * const int64_t us = watch.elapsedUs();
 * @endcode
 *
 * @par Thread Safety
 * A value: one owner.
 * @isrsafe
 */
class Stopwatch {
        public:
                /** @brief Starts timing. */
                Stopwatch() { restart(); }

                /** @brief Starts again from now. */
                void restart() {
                        _startUs = Detail::monotonicUs();
                        _startCycles = Detail::cycles();
                        return;
                }

                /** @brief Returns the microseconds since the start. */
                int64_t elapsedUs() const { return Detail::monotonicUs() - _startUs; }

                /** @brief Returns the CPU cycles since the start (see the class's limits). */
                uint32_t cycles() const { return Detail::cycles() - _startCycles; }

        private:
                int64_t  _startUs = 0;
                uint32_t _startCycles = 0;
};

BMDS_NAMESPACE_END
