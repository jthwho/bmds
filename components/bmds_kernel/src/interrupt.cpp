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

// Lost wakeup: a waiter publishes its semaphore (WaitList::add), then reads
// the flag; a raiser sets the flag, then releases the waiters. At least one
// sees the other (both sequentially consistent).

void Interrupt::raise() {
        _raises.fetch_add(1);
        _waiters.notify();
        return;
}

void Interrupt::raiseFromIsr() {
        _raises.fetch_add(1);
        _waiters.notifyFromIsr();
        return;
}

Error Interrupt::wait(Semaphore &semaphore, const Deadline &deadline, uint32_t seen) {
        Error err;
        {
                WaitList::Entry entry(_waiters, semaphore);
                err = raises() != seen ? Error(Error::Interrupted) : semaphore.acquire(deadline);
        }
        if (raises() != seen) return Error::Interrupted;
        return err;
}

Interrupt::Watch::Watch(Interrupt &interrupt, Semaphore &semaphore) : _entry(interrupt._waiters, semaphore) {}

Interrupt::Watch::~Watch() = default;

Interrupt *Interrupt::current() {
        return static_cast<Interrupt *>(Detail::taskInterrupt());
}

void Interrupt::setCurrent(Interrupt *interrupt) {
        Detail::setTaskInterrupt(interrupt);
        return;
}

Interrupt::Scope::Scope(Interrupt *interrupt) : _previous(current()) {
        setCurrent(interrupt);
}

Interrupt::Scope::~Scope() {
        setCurrent(_previous);
}

Error Interrupt::sleep(unsigned int delayMs) {
        // Nothing releases this semaphore but a raise.
        Semaphore never(1, 0);
        Deadline  deadline(delayMs);
        Error     err = wait(never, deadline);
        return err == Error::Timeout ? Error(Error::Ok) : err;
}

BMDS_NAMESPACE_END
