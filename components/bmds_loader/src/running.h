/**
 * @file      running.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program while it runs (target only): what its system calls reach
 * through the task's thread-local pointer, and how it says it's done.
 */

#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/config.h>
#include <bmds/process.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/** @brief The FreeRTOS thread-local pointer of a program's task that holds its `Running`. */
inline constexpr BaseType_t RunningTlsIndex = 1;
static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2,
              "programs need thread-local pointer 1 (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 2)");

struct Running {
                Process  *process = nullptr;
                uintptr_t entry = 0;  // the program's _start
                int32_t   status = 0; // set when it ends
                Semaphore done{1, 0}; // released when it ends

                /** @brief The running program of the calling task. */
                static Running &current() {
                        return *static_cast<Running *>(pvTaskGetThreadLocalStoragePointer(nullptr, RunningTlsIndex));
                }

                /**
                 * @brief Ends the calling program with @p status: signals the loader
                 *        and suspends; the loader then deletes the task.
                 */
                [[noreturn]] void finish(int32_t exitStatus) {
                        status = exitStatus;
                        done.release();
                        for (;;) vTaskSuspend(nullptr);
                }
};

BMDS_NAMESPACE_END
