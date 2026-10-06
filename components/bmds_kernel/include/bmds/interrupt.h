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
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An interrupt request (Ctrl-C, a signal): a flag that also wakes waiters.
 *
 * A console raises its Interrupt from its receive interrupt when it sees
 * Ctrl-C; a process's is raised when a signal is due for it. Code that
 * runs a job checks `isRaised()` between steps; blocking stream operations
 * wait through the calling task's (`current()`), which returns
 * `Error::Interrupted` as soon as it is raised, even while blocked. The
 * flag stays raised until `clear()` (the shell clears the console's when it
 * starts the next command).
 *
 * Waking works by releasing the semaphore each waiter is blocked on (a
 * `WaitList`), so up to `MaxWaiters` tasks can wait through one Interrupt
 * at a time (one per pipeline stage); a waiter beyond that still waits,
 * but uninterruptibly.
 * The releases may leave a semaphore with a spare count, so a semaphore
 * used with `wait()` must be an "event happened" signal whose takers
 * re-check their condition (the usual loop).
 *
 * It can also be **held** (a stopped process's, devplan/processes.md
 * §3.7.1): `wait()` and `sleep()` then park until it is resumed, neither
 * returning nor timing out meanwhile, and code between waits parks in
 * `park()`. A raise while held is seen once resumed.
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
                 * @brief Holds its waiters: they park until `resume()` (task context).
                 * @nonblocking
                 */
                void hold();

                /**
                 * @brief Ends a hold: parked waiters go on (task context).
                 * @nonblocking
                 */
                void resume();

                /**
                 * @brief Returns true while held.
                 * @isrsafe
                 */
                bool isHeld() const { return _held.load(); }

                /**
                 * @brief Waits while held (at once if not): for code between waits.
                 * @blocking
                 */
                void park();

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
                 * While held it parks: it returns only once resumed (a deadline that
                 * passed meanwhile then times out, a raise meanwhile interrupts).
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
                 * @brief Returns the calling task's interrupt: what its blocking
                 *        stream calls wake on (null: they don't).
                 *
                 * A program's task has its process's (raised by signals), the
                 * shell's tasks the console's (raised by Ctrl-C). Set with `Scope`.
                 */
                static Interrupt *current();

                /**
                 * @brief `wait()` on the calling task's interrupt (`current()`).
                 * @return `Ok`, `Timeout`, or `Interrupted`.
                 * @blocking
                 */
                static Error waitCurrent(Semaphore &semaphore, const Deadline &deadline) {
                        return wait(current(), semaphore, deadline);
                }

                /**
                 * @brief Makes an interrupt the calling task's (`current()`) for its
                 *        lifetime, then puts the previous one back.
                 */
                class Scope {
                        public:
                                explicit Scope(Interrupt *interrupt);
                                ~Scope();
                                Scope(const Scope &) = delete;
                                Scope &operator=(const Scope &) = delete;

                        private:
                                Interrupt *_previous;
                };

                /** @brief Sets the calling task's interrupt (`current()`) for good. */
                static void setCurrent(Interrupt *interrupt);

                /**
                 * @brief Releases a semaphore at every raise for its lifetime, so
                 *        a task waiting on it (through another interrupt) wakes on
                 *        this one's raises too. Takes a waiter slot (`MaxWaiters`);
                 *        without one, it watches nothing.
                 */
                class Watch {
                        public:
                                Watch(Interrupt &interrupt, Semaphore &semaphore);
                                ~Watch();
                                Watch(const Watch &) = delete;
                                Watch &operator=(const Watch &) = delete;

                        private:
                                WaitList::Entry _entry;
                };

                /**
                 * @brief Sleeps for @p delayMs, waking early if raised.
                 * @param delayMs How long; `WaitForever` sleeps until raised.
                 * @return `Ok` after the full delay, or `Interrupted`.
                 * @blocking
                 */
                Error sleep(unsigned int delayMs);

        private:
                std::atomic<uint32_t>  _raises{0}; // since clear(); raised while non-zero
                std::atomic<bool>      _held{false};
                WaitListOf<MaxWaiters> _waiters;
};

BMDS_NAMESPACE_END
