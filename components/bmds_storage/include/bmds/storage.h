/**
 * @file      storage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The storage service: owns the SD card's insertion and removal.
 *
 * One task (`storage`, PSRAM stack) waits on the card-detect interrupt.
 * Once the switch has been stable for 50 ms it attaches a card that went
 * in and mounts its FAT partitions (`Vfs::automount()`: `/sd0/p0`...), or
 * unmounts everything on one that came out and detaches it (open files go
 * stale: `NoMedia`). There is no polling. The first check happens in the task as well, so boot doesn't
 * wait for card initialization.
 *
 * @par Thread Safety
 * `init()` once at boot; the rest is thread-safe.
 */
namespace Storage {

        /**
         * @brief Boot stage: sets up the VFS, brings up the SD slot, registers
         *        it as a block device (`sd0`) and starts the service task.
         * @return `Ok`, or why it couldn't (the system runs on without storage).
         */
        Error init();

        /**
         * @brief Waits until the first card check is done: a card's partitions
         *        mounted, or none there (or it failed).
         * @param timeoutMs Timeout, or `WaitForever`.
         * @return `Ok`, `Timeout`, or `NotReady` if the service isn't running.
         * @blocking
         */
        Error waitForFirstMount(unsigned int timeoutMs);

} // namespace Storage

BMDS_NAMESPACE_END
