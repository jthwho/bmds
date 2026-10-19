/**
 * @file      semaphore.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A counting semaphore: its count an atomic word, a FreeRTOS semaphore
 *        only for waiting.
 *
 * With `maxCount` 1 it is a binary "event happened" flag, the usual way an
 * ISR wakes a waiting task. A take with a count there, and a give nobody
 * waits for, are one atomic operation; FreeRTOS is called only to wait and to
 * wake a waiter (devplan/bench.md §20). Waiters wake in FreeRTOS's order
 * (priority, then arrival), and a count given while some wait goes to them,
 * not to a newcomer. The control block is embedded in the object, so
 * constructing one never touches the heap. The header is FreeRTOS-free
 * (opaque storage) so portable kernel code builds in the host tests.
 *
 * @par Thread Safety
 * Thread-safe. `releaseFromIsr()` is ISR-safe.
 */
class Semaphore {
        public:
                /**
                 * @brief Creates a semaphore.
                 * @param maxCount     Maximum count (1 = binary).
                 * @param initialCount Starting count.
                 */
                explicit Semaphore(unsigned int maxCount, unsigned int initialCount);

                /** @brief Destroys the semaphore; nobody may be waiting on it. */
                ~Semaphore();

                Semaphore(const Semaphore &) = delete;
                Semaphore &operator=(const Semaphore &) = delete;

                /**
                 * @brief Takes one count, waiting up to @p timeoutMs.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                Error acquire(unsigned int timeoutMs);

                /**
                 * @brief Takes one count, waiting until @p deadline.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                Error acquire(const Deadline &deadline);

                /**
                 * @brief Takes one count if available.
                 * @return `Error::Ok`, or `Error::WouldBlock`.
                 * @nonblocking
                 */
                Error tryAcquire();

                /**
                 * @brief Adds one count (no effect when already at `maxCount`).
                 * @nonblocking
                 */
                void release();

                /**
                 * @brief Adds one count from an ISR.
                 *
                 * If this wakes a higher-priority task, the switch happens once,
                 * at interrupt exit (the ESP-IDF RISC-V port defers
                 * `portYIELD_FROM_ISR()` to the end of the ISR).
                 * @isrsafe
                 */
                void releaseFromIsr();

        private:
                alignas(Detail::SyncStorageAlign) unsigned char _storage[Detail::SyncStorageBytes];
                // The target's: the count, or minus the tasks waiting (the host's keeps its own).
                std::atomic<int32_t> _count{0};
                int32_t              _max = 0;
};

BMDS_NAMESPACE_END
