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
         * @brief Returns the calling core's cycle counter (the target's CPU
         *        cycles; nanoseconds on the host). Wraps: only a difference of
         *        two readings on one core means anything, and only across less
         *        than 2^32 cycles (about 11.9 s at 360 MHz).
         * @isrsafe
         */
        uint32_t cycles();

        /**
         * @brief Suspends task switching on this core (ISRs still run).
         *
         * For short sections that call into FreeRTOS, which a critical
         * section must not do. Pair with `resumeScheduler()`.
         */
        void suspendScheduler();

        /** @brief Undoes `suspendScheduler()`. */
        void resumeScheduler();

        /** @brief Returns the calling task's pointer for `Interrupt::current()` (null if none). */
        void *taskInterrupt();

        /** @brief Sets the calling task's pointer for `Interrupt::current()`. */
        void setTaskInterrupt(void *interrupt);

        /** @brief Returns the calling task's `TaskOwner` (null if none). */
        void *taskOwner();

        /** @brief Sets the calling task's `TaskOwner`. */
        void setTaskOwner(void *owner);

        /** @brief Returns task @p task's `TaskOwner` (null if none). @isrsafe */
        void *taskOwnerOf(void *task);

        /** @brief The system clock as carried across a reset (`Clock`). */
        struct ClockRecord {
                        int64_t wallMinusPersistentNs = 0; ///< Wall time minus `persistentTimeUs()` (in ns).
                        int32_t frequencyPpb = 0;          ///< The frequency correction.
        };

        /**
         * @brief Returns microseconds of a timer that keeps counting across
         *        resets (the RTC timer on the target; the monotonic clock on the host).
         */
        int64_t persistentTimeUs();

        /** @brief Reads the clock record that survived a reset; false if there is none. */
        bool loadClockRecord(ClockRecord &record);

        /** @brief Stores the clock record, to survive a reset. Task context, cheap. */
        void saveClockRecord(const ClockRecord &record);

        /** @brief Steps the C library's time (`time()`, `gettimeofday()`) to @p wallNs. */
        void setLibcTime(int64_t wallNs);

        /**
         * @brief Fills @p size bytes at @p data with random bytes (the
         *        hardware generator on the target). Task context.
         */
        void fillRandom(void *data, size_t size);

        /**
         * @brief Returns one word of the hardware generator at once: its register
         *        read, without `fillRandom()`'s pacing between words (each word
         *        of that takes ~56 µs, and its clock queries more:
         *        `devplan/programs.md` §19). For a word now and then. @isrsafe
         */
        uint32_t randomWord();

        /**
         * @brief Fills @p size bytes with true random bytes at boot: the
         *        target turns the hardware's main noise source (the SAR ADC)
         *        on for the moment. Boot only, before the ADC has another use.
         */
        void fillBootEntropy(void *data, size_t size);

} // namespace Detail

BMDS_NAMESPACE_END
