/**
 * @file      spinlock.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A cross-core critical section (`portMUX_TYPE`).
 *
 * Holding it disables interrupts on the local core and spins against the
 * other core, so hold it for a handful of instructions only: register
 * read-modify-writes and index updates. Never block, allocate or log while
 * holding it.
 *
 * @par Thread Safety
 * Thread-safe. Use `lockFromIsr()`/`unlockFromIsr()` in ISRs.
 */
class SpinLock {
        public:
                /** @brief RAII holder for task context. */
                class Locker {
                        public:
                                explicit Locker(SpinLock &lock) : _lock(lock) { _lock.lock(); }
                                ~Locker() { _lock.unlock(); }
                                Locker(const Locker &) = delete;
                                Locker &operator=(const Locker &) = delete;

                        private:
                                SpinLock &_lock;
                };

                constexpr SpinLock() = default;
                SpinLock(const SpinLock &) = delete;
                SpinLock &operator=(const SpinLock &) = delete;

                /** @brief Enters the critical section from a task. */
                void lock() {
                        portENTER_CRITICAL(&_mux);
                        return;
                }

                /** @brief Leaves the critical section from a task. */
                void unlock() {
                        portEXIT_CRITICAL(&_mux);
                        return;
                }

                /** @brief Enters the critical section from an ISR. @isrsafe */
                void lockFromIsr() {
                        portENTER_CRITICAL_ISR(&_mux);
                        return;
                }

                /** @brief Leaves the critical section from an ISR. @isrsafe */
                void unlockFromIsr() {
                        portEXIT_CRITICAL_ISR(&_mux);
                        return;
                }

        private:
                portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

BMDS_NAMESPACE_END
