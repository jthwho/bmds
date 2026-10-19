/**
 * @file      mutex.h
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
 * @brief A mutex with priority inheritance, not recursive.
 *
 * Uncontended, a lock and an unlock are a compare-and-swap each on the
 * holder's task handle (devplan/bench.md §21). A waiter raises the holder to
 * its own priority (FreeRTOS's inheritance) and sleeps on a FreeRTOS
 * semaphore; the holder is put back when it unlocks the last contended mutex
 * that raised it. Unlike a FreeRTOS mutex, a waiter that times out leaves the
 * holder raised until then (FreeRTOS lowers it to the next waiter's priority
 * at once). As with FreeRTOS's, a task woken by an unlock competes with one
 * arriving, and unlocking a mutex nobody holds is ignored (and logged). The wait's control block is embedded in the object, so
 * constructing a `Mutex` never touches the heap. The header is
 * FreeRTOS-free (opaque storage) so portable kernel code using it builds in
 * the host tests, where a `std::timed_mutex` stands in. Construction is a
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

                /** @brief Constructs an unlocked mutex. */
                Mutex();

                /** @brief Destroys the mutex; it must not be locked or waited on. */
                ~Mutex();

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
                Error lock(const Deadline &deadline);

                /**
                 * @brief Locks if free.
                 * @return `Error::Ok`, or `Error::WouldBlock`.
                 * @nonblocking
                 */
                Error tryLock();

                /** @brief Unlocks. Must be called by the holder. */
                void unlock();

        private:
                Error lockSlow(uintptr_t self, const Deadline &deadline);
                void  unlockSlow(uintptr_t self, void *caller);

                alignas(Detail::SyncStorageAlign) unsigned char _storage[Detail::SyncStorageBytes];
                // The target's (the host's keeps its own state in _storage):
                std::atomic<uintptr_t> _word{0};          // the holder's task handle, bit 0: contended
                void                  *_raised = nullptr; // the holder a waiter raised (under _spin)
                int32_t                _waiting = 0;      // tasks waiting (under _spin)
                alignas(4) unsigned char _spin[8];        // a portMUX_TYPE: the slow paths'
};

BMDS_NAMESPACE_END
