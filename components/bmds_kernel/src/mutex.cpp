/**
 * @file      mutex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/ticks.h>
#include <bmds/mutex.h>

BMDS_NAMESPACE_BEGIN

Error Mutex::lock(unsigned int timeoutMs) {
        return xSemaphoreTake(_handle, Detail::toTicks(timeoutMs)) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Mutex::lock(Deadline &deadline) {
        const TickType_t ticks = deadline.remainingTicks();
        if (ticks == 0) return Error::Timeout;
        return xSemaphoreTake(_handle, ticks) == pdTRUE ? Error() : Error(Error::Timeout);
}

Error Mutex::tryLock() {
        return xSemaphoreTake(_handle, 0) == pdTRUE ? Error() : Error(Error::WouldBlock);
}

BMDS_NAMESPACE_END
