/**
 * @file      mutex.h
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
 * @brief A FreeRTOS mutex (priority inheritance, not recursive).
 *
 * The control block is embedded in the object (`xSemaphoreCreateMutexStatic`),
 * so constructing a `Mutex` never touches the heap. Construction is a
 * runtime call: a kernel-global mutex lives in a `StaticObject` or inside an
 * object created at boot.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
class Mutex {
        public:
                /**
                 * @brief RAII lock holder.
                 *
                 * Check `isLocked()` when the timeout can expire.
                 */
                class Locker {
                        public:
                                /**
                                 * @brief Locks @p mutex, waiting up to @p timeoutMs.
                                 * @blocking
                                 */
                                Locker(Mutex &mutex, unsigned int timeoutMs)
                                    : _mutex(mutex), _locked(mutex.lock(timeoutMs).isOk()) {}

                                ~Locker() {
                                        if (_locked) _mutex.unlock();
                                }

                                Locker(const Locker &) = delete;
                                Locker &operator=(const Locker &) = delete;

                                /** @brief Returns true if the lock was acquired. */
                                bool isLocked() const { return _locked; }

                        private:
                                Mutex &_mutex;
                                bool   _locked;
                };

                Mutex() { _handle = xSemaphoreCreateMutexStatic(&_storage); }
                ~Mutex() { vSemaphoreDelete(_handle); }

                Mutex(const Mutex &) = delete;
                Mutex &operator=(const Mutex &) = delete;

                /**
                 * @brief Locks, waiting up to @p timeoutMs.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                Error lock(unsigned int timeoutMs);

                /**
                 * @brief Locks, waiting until @p deadline.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                Error lock(Deadline &deadline);

                /**
                 * @brief Locks if free.
                 * @return `Error::Ok`, or `Error::WouldBlock`.
                 * @nonblocking
                 */
                Error tryLock();

                /** @brief Unlocks. Must be called by the holder. */
                void unlock() {
                        xSemaphoreGive(_handle);
                        return;
                }

        private:
                StaticSemaphore_t _storage;
                SemaphoreHandle_t _handle;
};

BMDS_NAMESPACE_END
