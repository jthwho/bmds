/**
 * @file      semaphore.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/ticks.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

Error Semaphore::acquire(unsigned int timeoutMs) {
        return xSemaphoreTake(_handle, Detail::toTicks(timeoutMs)) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Semaphore::acquire(Deadline &deadline) {
        const TickType_t ticks = deadline.remainingTicks();
        if (ticks == 0) return Error::Timeout;
        return xSemaphoreTake(_handle, ticks) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Semaphore::tryAcquire() {
        return xSemaphoreTake(_handle, 0) == pdTRUE ? Error() : Error(Error::WouldBlock);
}

BMDS_NAMESPACE_END
