/**
 * @file      interrupt.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/platform.h>
#include <bmds/interrupt.h>

BMDS_NAMESPACE_BEGIN

// All atomics here are sequentially consistent; the two handshakes below
// depend on it.
//
// Lost wakeup: a waiter publishes its semaphore, then reads the flag; a
// raiser sets the flag, then reads the slots. At least one sees the other.
//
// Lifetime: a raiser counts itself in _busy before reading a slot, and a
// leaving waiter clears its slot and then waits for _busy to drain, so the
// semaphore (owned by a stream that may be destroyed next) is never released
// after its waiter has gone. The drain is short: a raiser in an ISR, or in a
// task with this core's scheduler suspended, can't be preempted by the
// waiter it is releasing.

void Interrupt::raise() {
        _raised.store(true);
        // FreeRTOS calls aren't allowed inside a critical section, so keep the
        // waiter from being scheduled in instead.
        Detail::suspendScheduler();
        wake(false);
        Detail::resumeScheduler();
        return;
}

void Interrupt::raiseFromIsr() {
        _raised.store(true);
        wake(true);
        return;
}

Error Interrupt::wait(Semaphore &semaphore, const Deadline &deadline) {
        size_t slot = MaxWaiters;
        for (size_t i = 0; i < MaxWaiters; ++i) {
                Semaphore *expected = nullptr;
                if (_waiters[i].compare_exchange_strong(expected, &semaphore)) {
                        slot = i;
                        break;
                }
        }
        Error err = isRaised() ? Error(Error::Interrupted) : semaphore.acquire(deadline);
        if (slot != MaxWaiters) {
                _waiters[slot].store(nullptr);
                while (_busy.load() != 0) {}
        }
        if (isRaised()) return Error::Interrupted;
        return err;
}

Error Interrupt::sleep(unsigned int delayMs) {
        // Nothing releases this semaphore but a raise.
        Semaphore never(1, 0);
        Deadline  deadline(delayMs);
        Error     err = wait(never, deadline);
        return err == Error::Timeout ? Error(Error::Ok) : err;
}

void Interrupt::wake(bool fromIsr) {
        _busy.fetch_add(1);
        for (std::atomic<Semaphore *> &waiter : _waiters) {
                Semaphore *semaphore = waiter.load();
                if (semaphore == nullptr) continue;
                if (fromIsr) {
                        semaphore->releaseFromIsr();
                } else {
                        semaphore->release();
                }
        }
        _busy.fetch_sub(1);
        return;
}

BMDS_NAMESPACE_END
