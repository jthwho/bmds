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
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The static-create handle is the address of the storage itself.
        SemaphoreHandle_t handleOf(unsigned char *storage) {
                return reinterpret_cast<SemaphoreHandle_t>(storage);
        }

} // namespace

Semaphore::Semaphore(unsigned int maxCount, unsigned int initialCount) {
        (void)xSemaphoreCreateCountingStatic(maxCount, initialCount, reinterpret_cast<StaticSemaphore_t *>(_storage));
}

Semaphore::~Semaphore() {
        vSemaphoreDelete(handleOf(_storage));
}

Error Semaphore::acquire(unsigned int timeoutMs) {
        return xSemaphoreTake(handleOf(_storage), Detail::toTicks(timeoutMs)) == pdTRUE ? Error()
                                                                                        : Error(Error::Timeout);
}

Error Semaphore::acquire(const Deadline &deadline) {
        const TickType_t ticks = Detail::toTicks(deadline);
        if (ticks == 0) return Error::Timeout;
        return xSemaphoreTake(handleOf(_storage), ticks) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Semaphore::tryAcquire() {
        return xSemaphoreTake(handleOf(_storage), 0) == pdTRUE ? Error() : Error(Error::WouldBlock);
}

void Semaphore::release() {
        xSemaphoreGive(handleOf(_storage));
        return;
}

void Semaphore::releaseFromIsr() {
        // If this wakes a higher-priority task, the switch happens once, at
        // interrupt exit (the RISC-V port defers portYIELD_FROM_ISR()).
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(handleOf(_storage), &woken);
        portYIELD_FROM_ISR(woken);
        return;
}

BMDS_NAMESPACE_END
