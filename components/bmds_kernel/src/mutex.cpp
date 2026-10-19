/**
 * @file      mutex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The mutex (devplan/bench.md §21). `_word` is the holder's task handle, 0 when free; bit 0 says
 * the slow paths are needed: tasks wait, or the holder was raised. Lock is a compare-and-swap
 * from 0 to the caller, unlock from the caller to 0; either fails only when bit 0 is set (or the
 * lock is held), and then takes `_spin`:
 *
 * - A lock that finds the mutex held sets bit 0, counts itself in `_waiting`, raises the holder
 *   to its priority (`xTaskPriorityInherit()`, which needs only the handle) and sleeps on the
 *   FreeRTOS semaphore in `_storage`, which only passes wakes. Woken, it tries again (a task
 *   arriving meanwhile may have taken it: FreeRTOS's mutex allows the same).
 * - An unlock with bit 0 frees the mutex (bit 0 kept while tasks wait, so arrivals take the slow
 *   path too), wakes one waiter, and puts its own priority back.
 *
 * Putting it back: FreeRTOS lowers a raised task in `xTaskPriorityDisinherit()` when its count
 * of mutexes held reaches 0, a count it keeps in a critical section at every take, which this
 * avoids. So each task's thread-local pointer 3 counts the mutexes whose waiters raised it
 * (`_raised`, one count a mutex), and its unlock of the last does FreeRTOS's count up and down,
 * lowering it then (or later, if it holds a FreeRTOS mutex too). Raising, counting and lowering
 * all happen under `raiseLock`, so a task raised for a second mutex is never lowered for the
 * first meanwhile. The holder can't end while a waiter raises it: both hold `_spin` then.
 */

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <riscv/rv_utils.h>
#include <bmds/detail/ticks.h>
#include <bmds/log.h>
#include <bmds/mutex.h>

// tasks.c's: the task running on each core.
extern "C" void *volatile pxCurrentTCBs[portNUM_PROCESSORS];

BMDS_NAMESPACE_BEGIN

namespace {

        static_assert(sizeof(StaticSemaphore_t) <= Detail::SyncStorageBytes, "update SyncStorageBytes");
        static_assert(alignof(StaticSemaphore_t) <= Detail::SyncStorageAlign, "update SyncStorageAlign");
        static_assert(sizeof(portMUX_TYPE) == 8 && alignof(portMUX_TYPE) <= 4, "Mutex::_spin holds a portMUX_TYPE");
        static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > 3, "Mutex needs thread-local pointer 3");

        constexpr uintptr_t  Contended = 1;
        constexpr uintptr_t  NoTask = 4; // the holder before the scheduler runs (not a TCB's address)
        constexpr BaseType_t RaisedTlsIndex = 3;

        portMUX_TYPE raiseLock = portMUX_INITIALIZER_UNLOCKED;

        SemaphoreHandle_t handleOf(unsigned char *storage) {
                return reinterpret_cast<SemaphoreHandle_t>(storage);
        }

        portMUX_TYPE *spinOf(unsigned char *spin) {
                return reinterpret_cast<portMUX_TYPE *>(spin);
        }

        // The calling task's handle: MIE off so it can't move to the other core between reading
        // its core and the slot (xTaskGetCurrentTaskHandle() masks by the threshold, dearer).
        FORCE_INLINE_ATTR uintptr_t currentTask() {
                const uint32_t status = RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
                void          *task = pxCurrentTCBs[rv_utils_get_core_id()];
                RV_SET_CSR(mstatus, status & MSTATUS_MIE);
                return task != nullptr ? reinterpret_cast<uintptr_t>(task) : NoTask;
        }

        // Adds @p delta to the count of mutexes that raised @p task (raiseLock held): the new count.
        intptr_t countRaised(void *task, intptr_t delta) {
                const auto     handle = static_cast<TaskHandle_t>(task);
                const intptr_t n =
                        reinterpret_cast<intptr_t>(pvTaskGetThreadLocalStoragePointer(handle, RaisedTlsIndex)) + delta;
                vTaskSetThreadLocalStoragePointer(handle, RaisedTlsIndex, reinterpret_cast<void *>(n));
                return n;
        }

} // namespace

Mutex::Mutex() {
        const portMUX_TYPE unlocked = portMUX_INITIALIZER_UNLOCKED;
        std::memcpy(_spin, &unlocked, sizeof(unlocked));
        // Up to one wake for each task that may wait.
        (void)xSemaphoreCreateCountingStatic(0x7fff, 0, reinterpret_cast<StaticSemaphore_t *>(_storage));
}

Mutex::~Mutex() {
        vSemaphoreDelete(handleOf(_storage));
}

Error Mutex::lock(unsigned int timeoutMs) {
        const uintptr_t self = currentTask();
        uintptr_t       free = 0;
        if (_word.compare_exchange_strong(free, self, std::memory_order_acquire, std::memory_order_relaxed))
                return Error();
        return lockSlow(self, Deadline(timeoutMs));
}

Error Mutex::lock(const Deadline &deadline) {
        const uintptr_t self = currentTask();
        uintptr_t       free = 0;
        if (_word.compare_exchange_strong(free, self, std::memory_order_acquire, std::memory_order_relaxed))
                return Error();
        return lockSlow(self, deadline);
}

Error Mutex::tryLock() {
        const uintptr_t self = currentTask();
        uintptr_t       w = 0;
        if (_word.compare_exchange_strong(w, self, std::memory_order_acquire, std::memory_order_relaxed))
                return Error();
        if ((w & ~Contended) != 0) return Error::WouldBlock;
        // Free, but tasks wait: taken as the slow path takes it.
        portMUX_TYPE *spin = spinOf(_spin);
        portENTER_CRITICAL(spin);
        w = _word.load(std::memory_order_acquire);
        const bool taken =
                (w & ~Contended) == 0 &&
                _word.compare_exchange_strong(w, self | (_waiting > 0 ? Contended : 0), std::memory_order_acquire,
                                              std::memory_order_relaxed); // as lockSlow()'s
        portEXIT_CRITICAL(spin);
        return taken ? Error() : Error(Error::WouldBlock);
}

Error Mutex::lockSlow(uintptr_t self, const Deadline &deadline) {
        portMUX_TYPE *spin = spinOf(_spin);
        for (;;) {
                portENTER_CRITICAL(spin);
                uintptr_t w = _word.load(std::memory_order_acquire);
                if ((w & ~Contended) == 0) {
                        // Free: ours, bit 0 kept while others wait. A compare-and-swap, not a store:
                        // with bit 0 clear a lock's fast path (which doesn't take _spin) may take it
                        // meanwhile, and a store would make two holders.
                        const bool taken =
                                _word.compare_exchange_strong(w, self | (_waiting > 0 ? Contended : 0),
                                                              std::memory_order_acquire, std::memory_order_relaxed);
                        portEXIT_CRITICAL(spin);
                        if (taken) return Error();
                        continue;
                }
                const TickType_t ticks = Detail::toTicks(deadline);
                if (ticks == 0) {
                        portEXIT_CRITICAL(spin);
                        return Error::Timeout;
                }
                // Bit 0 set before waiting: the holder's unlock fails its compare-and-swap then.
                if ((w & Contended) == 0 &&
                    !_word.compare_exchange_strong(w, w | Contended, std::memory_order_relaxed)) {
                        portEXIT_CRITICAL(spin); // the holder left meanwhile
                        continue;
                }
                const uintptr_t holder = w & ~Contended;
                ++_waiting;
                if (holder != NoTask && holder != self) {
                        portENTER_CRITICAL(&raiseLock);
                        (void)xTaskPriorityInherit(reinterpret_cast<TaskHandle_t>(holder));
                        if (_raised != reinterpret_cast<void *>(holder)) {
                                _raised = reinterpret_cast<void *>(holder);
                                (void)countRaised(_raised, 1);
                        }
                        portEXIT_CRITICAL(&raiseLock);
                }
                portEXIT_CRITICAL(spin);
                const bool woken = xSemaphoreTake(handleOf(_storage), ticks) == pdTRUE;
                portENTER_CRITICAL(spin);
                --_waiting;
                portEXIT_CRITICAL(spin);
                // Woken: try again. Timed out: one last look (a wake given as the time ran out is
                // left in the semaphore: a later waiter wakes once for nothing and waits again).
                (void)woken;
        }
}

void Mutex::unlock() {
        const uintptr_t self = currentTask();
        uintptr_t       held = self;
        if (_word.compare_exchange_strong(held, 0, std::memory_order_release, std::memory_order_relaxed)) return;
        unlockSlow(self, __builtin_return_address(0));
        return;
}

void Mutex::unlockSlow(uintptr_t self, void *caller) {
        portMUX_TYPE *spin = spinOf(_spin);
        portENTER_CRITICAL(spin);
        const uintptr_t holder = _word.load(std::memory_order_relaxed) & ~Contended;
        if (holder == 0) {
                // Not locked: ignored, as FreeRTOS's mutex ignores a give with no holder (its owner
                // check allows that case), but it is a caller's mistake: told once, with where.
                portEXIT_CRITICAL(spin);
                static std::atomic<bool> told{false};
                if (!told.exchange(true)) bmdsWarn("Mutex %p unlocked while not locked, from %p", this, caller);
                return;
        }
        configASSERT(holder == self); // only its holder unlocks it, as FreeRTOS checks
        BaseType_t lowered = pdFALSE;
        if (_raised != nullptr) {
                portENTER_CRITICAL(&raiseLock);
                // The last mutex that raised it: back to its own priority. FreeRTOS lowers a task
                // when its count of mutexes held reaches 0: up one and down one does it (or not yet,
                // if it holds a FreeRTOS mutex too, whose give lowers it).
                if (countRaised(_raised, -1) == 0 && reinterpret_cast<uintptr_t>(_raised) == self) {
                        (void)pvTaskIncrementMutexHeldCount();
                        lowered = xTaskPriorityDisinherit(reinterpret_cast<TaskHandle_t>(self));
                }
                _raised = nullptr;
                portEXIT_CRITICAL(&raiseLock);
        }
        const int32_t waiting = _waiting;
        _word.store(waiting > 0 ? Contended : 0, std::memory_order_release);
        portEXIT_CRITICAL(spin);
        if (waiting > 0) xSemaphoreGive(handleOf(_storage)); // one of them tries again
        if (lowered) taskYIELD();                            // a task it held off may run now
        return;
}

BMDS_NAMESPACE_END
