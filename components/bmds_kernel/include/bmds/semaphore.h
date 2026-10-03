/**
 * @file      semaphore.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <bmds/config.h>
#include <bmds/deadline.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A FreeRTOS counting semaphore.
 *
 * With `maxCount` 1 it is a binary "event happened" flag, the usual way an
 * ISR wakes a waiting task. The control block is embedded in the object, so
 * constructing one never touches the heap.
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
                explicit Semaphore(unsigned int maxCount, unsigned int initialCount) {
                        _handle = xSemaphoreCreateCountingStatic(maxCount, initialCount, &_storage);
                }
                ~Semaphore() { vSemaphoreDelete(_handle); }

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
                Error acquire(Deadline &deadline);

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
                void release() {
                        xSemaphoreGive(_handle);
                        return;
                }

                /**
                 * @brief Adds one count from an ISR.
                 *
                 * If this wakes a higher-priority task, the switch happens once,
                 * at interrupt exit (the ESP-IDF RISC-V port defers
                 * `portYIELD_FROM_ISR()` to the end of the ISR).
                 * @isrsafe
                 */
                void releaseFromIsr() {
                        BaseType_t woken = pdFALSE;
                        xSemaphoreGiveFromISR(_handle, &woken);
                        portYIELD_FROM_ISR(woken);
                        return;
                }

        private:
                StaticSemaphore_t _storage;
                SemaphoreHandle_t _handle;
};

BMDS_NAMESPACE_END
