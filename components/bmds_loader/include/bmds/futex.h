/**
 * @file      futex.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/deadline.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Program threads waiting on words in memory: the `futex` system call
 *        (devplan/processes.md §3.10), which musl's locks, condition
 *        variables and joins are built on.
 *
 * A waiter sleeps only while its word still holds the value it expects, the
 * comparison and the queueing made under the lock that wakes take: a thread
 * that changes the word and then wakes can't miss one that is going to
 * sleep. Waiters are keyed by the word's address (there is one address
 * space) and woken first come, first served. Each waiter's record is on its
 * own stack: nothing is allocated.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class FutexTable {
        public:
                FutexTable() = default;
                FutexTable(const FutexTable &) = delete;
                FutexTable &operator=(const FutexTable &) = delete;

                /**
                 * @brief Waits while `*word == value`, on @p interrupt (null: not
                 *        interruptible), until woken or @p deadline.
                 * @return `Ok` (woken), `WouldBlock` (the word held another value),
                 *         `Timeout`, or `Interrupted`.
                 * @blocking
                 */
                Error wait(const volatile int32_t *word, int32_t value, const Deadline &deadline, Interrupt *interrupt);

                /**
                 * @brief Wakes up to @p count waiters on @p word (the longest waiting first).
                 * @return How many were woken.
                 */
                int wake(const volatile int32_t *word, int count);

                /**
                 * @brief Wakes up to @p count waiters on @p word and moves up to
                 *        @p moveCount more to wait on @p to.
                 * @return How many were woken.
                 */
                int requeue(const volatile int32_t *word, int count, const volatile int32_t *to, int moveCount);

                /** @brief Returns how many wait on @p word (tests). */
                int waiters(const volatile int32_t *word);

        private:
                struct Waiter {
                                const volatile int32_t *word;
                                Semaphore               woken{1, 0};
                                Waiter                 *next = nullptr;
                                bool                    queued = true;
                };

                void remove(Waiter &w); // under _lock

                Mutex   _lock;
                Waiter *_first = nullptr; // in the order they came
};

BMDS_NAMESPACE_END
