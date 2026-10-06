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

namespace {

        // How often a parked waiter without a slot in the list looks at the hold.
        constexpr unsigned int UnwatchedParkMs = 10;

} // namespace

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

void Interrupt::hold() {
        _held.store(true);
        _waiters.notify();
        return;
}

void Interrupt::resume() {
        _held.store(false);
        _waiters.notify();
        return;
}

void Interrupt::park() {
        // As for a raise: the waiter publishes its semaphore, then reads the hold.
        while (_held.load()) {
                Semaphore       wake(1, 0);
                WaitList::Entry entry(_waiters, wake);
                if (!_held.load()) break;
                // Without a slot nothing releases it: look again now and then.
                (void)wake.acquire(entry.isAdded() ? WaitForever : UnwatchedParkMs);
        }
        return;
}

Error Interrupt::wait(Semaphore &semaphore, const Deadline &deadline, uint32_t seen) {
        for (;;) {
                park();
                Error err;
                bool  waited = false;
                {
                        WaitList::Entry entry(_waiters, semaphore);
                        if (raises() == seen && !_held.load()) {
                                err = semaphore.acquire(deadline);
                                waited = true;
                        }
                }
                if (_held.load()) {
                        // Held meanwhile: what was taken is taken after the hold.
                        if (waited && err.isOk()) semaphore.release();
                        continue;
                }
                if (raises() != seen) return Error::Interrupted;
                if (!waited) continue; // held, and resumed already
                return err;
        }
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
