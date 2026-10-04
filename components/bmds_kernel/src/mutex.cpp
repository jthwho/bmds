/**
 * @file      mutex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <bmds/detail/ticks.h>
#include <bmds/mutex.h>

BMDS_NAMESPACE_BEGIN

namespace {

        static_assert(sizeof(StaticSemaphore_t) == Detail::SyncStorageBytes, "update SyncStorageBytes");
        static_assert(alignof(StaticSemaphore_t) == Detail::SyncStorageAlign, "update SyncStorageAlign");

        // The static-create handle is the address of the storage itself.
        SemaphoreHandle_t handleOf(unsigned char *storage) {
                return reinterpret_cast<SemaphoreHandle_t>(storage);
        }

} // namespace

Mutex::Mutex() {
        (void)xSemaphoreCreateMutexStatic(reinterpret_cast<StaticSemaphore_t *>(_storage));
}

Mutex::~Mutex() {
        vSemaphoreDelete(handleOf(_storage));
}

Error Mutex::lock(unsigned int timeoutMs) {
        return xSemaphoreTake(handleOf(_storage), Detail::toTicks(timeoutMs)) == pdTRUE ? Error()
                                                                                        : Error(Error::Timeout);
}

Error Mutex::lock(const Deadline &deadline) {
        const TickType_t ticks = Detail::toTicks(deadline);
        if (ticks == 0) return Error::Timeout;
        return xSemaphoreTake(handleOf(_storage), ticks) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Mutex::tryLock() {
        return xSemaphoreTake(handleOf(_storage), 0) == pdTRUE ? Error() : Error(Error::WouldBlock);
}

void Mutex::unlock() {
        xSemaphoreGive(handleOf(_storage));
        return;
}

BMDS_NAMESPACE_END
