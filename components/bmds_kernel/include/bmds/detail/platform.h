/**
 * @file      platform.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The small platform layer under the portable kernel classes: declared
 * here, defined once for the target (FreeRTOS) and once for the host tests
 * (std::thread primitives).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace Detail {

#if BMDS_PLATFORM_ESP
        /** @brief Bytes of opaque storage in a Mutex or Semaphore (`StaticSemaphore_t`). */
        inline constexpr size_t SyncStorageBytes = 92;
        /** @brief Alignment of that storage. */
        inline constexpr size_t SyncStorageAlign = 4;
#else
        inline constexpr size_t SyncStorageBytes = 128;
        inline constexpr size_t SyncStorageAlign = 8;
#endif

        /**
         * @brief Returns a monotonic time in microseconds.
         * @isrsafe
         */
        int64_t monotonicUs();

        /**
         * @brief Suspends task switching on this core (ISRs still run).
         *
         * For short sections that call into FreeRTOS, which a critical
         * section must not do. Pair with `resumeScheduler()`.
         */
        void suspendScheduler();

        /** @brief Undoes `suspendScheduler()`. */
        void resumeScheduler();

} // namespace Detail

BMDS_NAMESPACE_END
