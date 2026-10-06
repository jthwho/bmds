/**
 * @file      futex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/futex.h>

BMDS_NAMESPACE_BEGIN

Error FutexTable::wait(const volatile int32_t *word, int32_t value, const Deadline &deadline, Interrupt *interrupt) {
        Waiter w{word};
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (*word != value) return Error::WouldBlock;
                Waiter **at = &_first;
                while (*at != nullptr) at = &(*at)->next;
                *at = &w;
        }
        const Error err = Interrupt::wait(interrupt, w.woken, deadline);
        // A waker takes it off the list and releases it under the lock: off the list, it was
        // woken, whatever the wait said.
        Mutex::Locker locker(_lock, WaitForever);
        if (!w.queued) return Error::Ok;
        remove(w);
        return err;
}

int FutexTable::wake(const volatile int32_t *word, int count) {
        return requeue(word, count, nullptr, 0);
}

int FutexTable::requeue(const volatile int32_t *word, int count, const volatile int32_t *to, int moveCount) {
        Mutex::Locker locker(_lock, WaitForever);
        int           woken = 0;
        int           moved = 0;
        for (Waiter *w = _first; w != nullptr;) {
                Waiter *next = w->next;
                if (w->word == word) {
                        if (woken < count) {
                                remove(*w);
                                w->woken.release();
                                ++woken;
                        } else if (to != nullptr && moved < moveCount) {
                                w->word = to;
                                ++moved;
                        } else {
                                break;
                        }
                }
                w = next;
        }
        return woken;
}

int FutexTable::waiters(const volatile int32_t *word) {
        Mutex::Locker locker(_lock, WaitForever);
        int           n = 0;
        for (const Waiter *w = _first; w != nullptr; w = w->next) n += w->word == word ? 1 : 0;
        return n;
}

void FutexTable::remove(Waiter &w) {
        for (Waiter **at = &_first; *at != nullptr; at = &(*at)->next) {
                if (*at != &w) continue;
                *at = w.next;
                break;
        }
        w.next = nullptr;
        w.queued = false;
        return;
}

BMDS_NAMESPACE_END
