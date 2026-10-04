/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

int64_t Detail::monotonicUs() {
        return esp_timer_get_time();
}

void Detail::suspendScheduler() {
        vTaskSuspendAll();
        return;
}

void Detail::resumeScheduler() {
        (void)xTaskResumeAll();
        return;
}

BMDS_NAMESPACE_END
