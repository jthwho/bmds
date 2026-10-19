/**
 * @file      semaphore.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <bmds/detail/ticks.h>
#include <bmds/placement.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The static-create handle is the address of the storage itself.
        SemaphoreHandle_t handleOf(unsigned char *storage) {
                return reinterpret_cast<SemaphoreHandle_t>(storage);
        }

} // namespace

// The count is _count (devplan/bench.md §20): above 0 the counts there, at or below 0 minus the
// tasks waiting. A take decrements it; one that took it to or below 0 waits on the FreeRTOS
// semaphore, which only passes wakes: a give that found it below 0 gives that, else only
// increments it (up to _max). A waiter that times out takes its place back unless a give already
// counted it (the count no longer below 0): that give's wake is then its, and it takes it.

Semaphore::Semaphore(unsigned int maxCount, unsigned int initialCount) {
        _max = int32_t(maxCount);
        _count.store(int32_t(initialCount < maxCount ? initialCount : maxCount), std::memory_order_relaxed);
        // Up to one wake for each task that may wait.
        (void)xSemaphoreCreateCountingStatic(0x7fff, 0, reinterpret_cast<StaticSemaphore_t *>(_storage));
}

Semaphore::~Semaphore() {
        vSemaphoreDelete(handleOf(_storage));
}

namespace {

        // One count if there is one, without waiting.
        inline bool takeCount(std::atomic<int32_t> &count) {
                int32_t c = count.load(std::memory_order_relaxed);
                while (c > 0)
                        if (count.compare_exchange_weak(c, c - 1, std::memory_order_acquire, std::memory_order_relaxed))
                                return true;
                return false;
        }

        // Waits for a count for up to @p ticks (FreeRTOS's), the caller registered as a waiter.
        Error waitForCount(SemaphoreHandle_t wake, std::atomic<int32_t> &count, TickType_t ticks) {
                if (xSemaphoreTake(wake, ticks) == pdTRUE) return Error();
                // Gone: our place back, unless a give counted us already (its wake is coming, ours).
                int32_t c = count.load(std::memory_order_relaxed);
                while (c < 0)
                        if (count.compare_exchange_weak(c, c + 1, std::memory_order_relaxed)) return Error::Timeout;
                (void)xSemaphoreTake(wake, portMAX_DELAY);
                return Error();
        }

} // namespace

Error Semaphore::acquire(unsigned int timeoutMs) {
        if (takeCount(_count)) return Error();
        if (_count.fetch_sub(1, std::memory_order_acquire) > 0) return Error(); // one came meanwhile
        return waitForCount(handleOf(_storage), _count, Detail::toTicks(timeoutMs));
}

Error Semaphore::acquire(const Deadline &deadline) {
        if (takeCount(_count)) return Error();
        const TickType_t ticks = Detail::toTicks(deadline);
        if (ticks == 0) return Error::Timeout;
        if (_count.fetch_sub(1, std::memory_order_acquire) > 0) return Error();
        return waitForCount(handleOf(_storage), _count, ticks);
}

Error Semaphore::tryAcquire() {
        return takeCount(_count) ? Error() : Error(Error::WouldBlock);
}

void Semaphore::release() {
        int32_t c = _count.load(std::memory_order_relaxed);
        do {
                if (c >= _max) return; // full: no effect
        } while (!_count.compare_exchange_weak(c, c + 1, std::memory_order_release, std::memory_order_relaxed));
        if (c < 0) xSemaphoreGive(handleOf(_storage)); // a waiter's
        return;
}

BMDS_ISR_ATTR void Semaphore::releaseFromIsr() {
        int32_t c = _count.load(std::memory_order_relaxed);
        do {
                if (c >= _max) return;
        } while (!_count.compare_exchange_weak(c, c + 1, std::memory_order_release, std::memory_order_relaxed));
        if (c < 0) {
                // If this wakes a higher-priority task, the switch happens once, at
                // interrupt exit (the RISC-V port defers portYIELD_FROM_ISR()).
                BaseType_t woken = pdFALSE;
                xSemaphoreGiveFromISR(handleOf(_storage), &woken);
                portYIELD_FROM_ISR(woken);
        }
        return;
}

BMDS_NAMESPACE_END
