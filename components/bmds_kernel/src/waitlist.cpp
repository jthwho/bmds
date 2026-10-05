/**
 * @file      waitlist.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/platform.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

// All atomics here are sequentially consistent; the two handshakes below
// depend on it.
//
// Lost wakeup: a waiter publishes its semaphore, then checks its condition;
// a notifier changes the condition, then reads the slots (after a full
// fence, since the condition may be a plain or release store). At least one
// sees the other.
//
// Lifetime: a notifier counts itself in _busy before reading a slot, and a
// leaving waiter clears its slot and then waits for _busy to drain, so the
// semaphore (often on the waiter's stack) is never released after its
// waiter has gone. The drain is short: a notifier in an ISR, or in a task
// with this core's scheduler suspended, can't be preempted by the waiter it
// is releasing.

size_t WaitList::add(Semaphore &semaphore) {
        for (size_t i = 0; i < _count; ++i) {
                Semaphore *expected = nullptr;
                if (_slots[i].compare_exchange_strong(expected, &semaphore)) return i;
        }
        return NoSlot;
}

void WaitList::remove(size_t slot) {
        if (slot == NoSlot) return;
        _slots[slot].store(nullptr);
        while (_busy.load() != 0) {}
        return;
}

bool WaitList::hasWaiters() const {
        for (size_t i = 0; i < _count; ++i) {
                if (_slots[i].load() != nullptr) return true;
        }
        return false;
}

void WaitList::notify() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!hasWaiters()) return;
        // FreeRTOS calls aren't allowed inside a critical section, so keep the
        // waiter from being scheduled in instead.
        Detail::suspendScheduler();
        release(false);
        Detail::resumeScheduler();
        return;
}

void WaitList::notifyFromIsr() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        release(true);
        return;
}

void WaitList::release(bool fromIsr) {
        _busy.fetch_add(1);
        for (size_t i = 0; i < _count; ++i) {
                Semaphore *semaphore = _slots[i].load();
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
