/**
 * @file      sysmon.h
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
 * @brief The system monitor's own part of the kernel (`devplan/sysmon.md`):
 *        the tasks' CPU time (`CpuTime`), the system-wide counters (the
 *        cores, memory, the clock, the system) in the registry (`Stats`), and
 *        the settings `sysmon/interval` and `sysmon/limits`, followed live.
 *
 * @par Thread Safety
 * `init()` once, at boot.
 */
namespace Sysmon {

        /** @brief Boot stage, after `sysconf`. @return `Ok`, or what failed. */
        Error init();

} // namespace Sysmon

BMDS_NAMESPACE_END
