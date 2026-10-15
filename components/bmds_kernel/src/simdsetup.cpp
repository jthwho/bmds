/**
 * @file      simdsetup.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <freertos/FreeRTOS.h>
#include <bmds/semaphore.h>
#include <bmds/simd.h>
#include <bmds/simdsetup.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

Error Simd::setUp() {
#if BMDS_HAS_PIE
        // A short task pinned to each core: its first PIE instruction traps to turn PIE on and
        // takes PIE's save area from its stack (the IPC tasks' stacks are too small to give it),
        // freed with the task. The register is the core's and stays set.
        for (int core = 0; core < portNUM_PROCESSORS; ++core) {
                Semaphore    done(1, 0);
                Task         task;
                Task::Config config;
                config.name = "simd";
                config.stackBytes = 2048;
                config.priority = configMAX_PRIORITIES - 1;
                config.core = core;
                Error err = task.start(
                        config,
                        [](void *arg) {
                                const uint32_t misaligned = 3; // bit 1 loads, bit 0 stores
                                asm volatile("esp.movx.w.cfg %0" ::"cr"(misaligned));
                                static_cast<Semaphore *>(arg)->release();
                                for (;;) Task::sleep(WaitForever); // stopped below
                        },
                        &done);
                if (err.isOk()) err = done.acquire(WaitForever);
                if (task.isRunning()) (void)task.stop();
                if (err.isError()) return err;
        }
#endif
        return Error::Ok;
}

BMDS_NAMESPACE_END
