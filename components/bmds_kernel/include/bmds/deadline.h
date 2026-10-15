/**
 * @file      deadline.h
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
                /** @brief `remainingUs()` of a deadline that never expires. */
                static constexpr int64_t Forever = INT64_MAX;

                /** @brief Starts the clock. */
                explicit Deadline(unsigned int timeoutMs)
                    : _expiresUs(timeoutMs == WaitForever
                                         ? Forever
                                         : Detail::monotonicUs() + static_cast<int64_t>(timeoutMs) * 1000) {}

                Deadline(const Deadline &) = delete;
                Deadline &operator=(const Deadline &) = delete;

                /** @brief Returns true for a `WaitForever` deadline. */
                bool isForever() const { return _expiresUs == Forever; }

                /**
                 * @brief Returns the microseconds left: 0 once expired, `Forever` for `WaitForever`.
                 *        Check for 0 before making a timeout of it: 0 ms is `WaitForever`.
                 */
                int64_t remainingUs() const {
                        if (isForever()) return Forever;
                        const int64_t left = _expiresUs - Detail::monotonicUs();
                        return left > 0 ? left : 0;
                }

                /** @brief Returns true once the timeout has passed. */
                bool isExpired() const { return remainingUs() == 0; }

        private:
                int64_t _expiresUs;
};

BMDS_NAMESPACE_END
