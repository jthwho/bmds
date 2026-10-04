/**
 * @file      interrupt.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/deadline.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A job's interrupt request (Ctrl-C): a flag that also wakes waiters.
 *
 * A console raises its Interrupt from its receive interrupt when it sees
 * Ctrl-C. Code that runs a job checks `isRaised()` between steps; blocking
 * stream operations wait through `wait()`, which returns
 * `Error::Interrupted` as soon as the interrupt is raised, even while
 * blocked. The flag stays raised until `clear()` (the shell clears it when
 * it starts the next command).
 *
 * Waking works by releasing the semaphore each waiter is blocked on, so up
 * to `MaxWaiters` tasks can wait through one Interrupt at a time (one per
 * pipeline stage); a waiter beyond that still waits, but uninterruptibly.
 * The releases may leave a semaphore with a spare count, so a semaphore
 * used with `wait()` must be an "event happened" signal whose takers
 * re-check their condition (the usual loop).
 *
 * No allocation, no FreeRTOS objects of its own; constant-initializable.
 *
 * @par Thread Safety
 * Thread-safe. `raiseFromIsr()` and `isRaised()` are ISR-safe.
 */
class Interrupt {
        public:
                /** @brief Tasks that can wait through one Interrupt at the same time. */
                static constexpr size_t MaxWaiters = 8;

                /** @brief Constructs a clear interrupt. */
                constexpr Interrupt() = default;

                Interrupt(const Interrupt &) = delete;
                Interrupt &operator=(const Interrupt &) = delete;

                /**
                 * @brief Returns true if the interrupt has been raised and not cleared.
                 * @isrsafe
                 */
                bool isRaised() const { return _raises.load() != 0; }

                /**
                 * @brief Returns how often it has been raised since the last `clear()`.
                 * @isrsafe
                 */
                uint32_t raises() const { return _raises.load(); }

                /**
                 * @brief Raises the interrupt and wakes every waiter (task context).
                 * @nonblocking
                 */
                void raise();

                /**
                 * @brief Raises the interrupt and wakes every waiter (ISR context).
                 * @isrsafe
                 */
                void raiseFromIsr();

                /** @brief Clears the flag (and the count). */
                void clear() { _raises.store(0); }

                /**
                 * @brief Takes @p semaphore, giving up at @p deadline or when raised.
                 * @return `Ok`, `Timeout`, or `Interrupted` (also if already raised).
                 * @blocking
                 */
                Error wait(Semaphore &semaphore, const Deadline &deadline) { return wait(semaphore, deadline, 0); }

                /**
                 * @brief Takes @p semaphore, giving up at @p deadline or when raised
                 *        again: when `raises()` is no longer @p seen.
                 *
                 * For a waiter that has handled the raises so far and waits for
                 * the next (with @p seen 0, the same as the plain `wait()`).
                 *
                 * @return `Ok`, `Timeout`, or `Interrupted` (also if already past @p seen).
                 * @blocking
                 */
                Error wait(Semaphore &semaphore, const Deadline &deadline, uint32_t seen);

                /**
                 * @brief `wait()` on @p interrupt, or a plain acquire when it is null.
                 * @return `Ok`, `Timeout`, or `Interrupted`.
                 * @blocking
                 */
                static Error wait(Interrupt *interrupt, Semaphore &semaphore, const Deadline &deadline) {
                        return interrupt != nullptr ? interrupt->wait(semaphore, deadline)
                                                    : semaphore.acquire(deadline);
                }

                /**
                 * @brief Sleeps for @p delayMs, waking early if raised.
                 * @param delayMs How long; `WaitForever` sleeps until raised.
                 * @return `Ok` after the full delay, or `Interrupted`.
                 * @blocking
                 */
                Error sleep(unsigned int delayMs);

        private:
                void wake(bool fromIsr);

                std::atomic<uint32_t>    _raises{0}; // since clear(); raised while non-zero
                std::atomic<uint32_t>    _busy{0}; // raisers currently releasing waiters
                std::atomic<Semaphore *> _waiters[MaxWaiters] = {};
};

BMDS_NAMESPACE_END
