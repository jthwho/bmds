/**
 * @file      cputime.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tasks' CPU time (`devplan/sysmon.md` §3.1): FreeRTOS's run-time
 *        stats, counted on the ESP timer (1 µs) while `sysmon/enable` is on.
 *
 * FreeRTOS reads its run-time clock at every context switch; BMDS routes that
 * read (the linker's `--wrap`) through a flag set once at boot, so with the
 * setting off the clock reads 0 and no task is charged anything. The setting
 * applies at the next boot: turning the clock on mid-run would charge the
 * task running then with all the time before.
 *
 * @par Thread Safety
 * `init()` once (the boot stage); `isEnabled()` from anywhere.
 */
namespace CpuTime {

        /** @brief Boot stage, after `sysconf`: reads `sysmon/enable`. @return `Ok`. */
        Error init();

        /** @brief Returns true if CPU time is counted this boot. */
        bool isEnabled();

        /** @brief Returns the cores. */
        int cores();

        /**
         * @brief Returns the time @p core has been busy (not running its idle
         *        task) since boot, in µs, including the idle stretch running
         *        now; -1 if CPU time isn't counted. Never goes back.
         *
         * One caller at a time (the counters' registry).
         */
        int64_t busyUs(int core);

} // namespace CpuTime

BMDS_NAMESPACE_END
