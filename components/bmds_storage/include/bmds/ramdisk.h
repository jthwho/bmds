/**
 * @file      ramdisk.h
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
 * @brief The RAM disk: scratch files without a card (`/ram0/p0`).
 *
 * A sparse `RamBlockDevice` in PSRAM registered as `ram0`, formatted and
 * mounted at boot. Its size (`CONFIG_BMDS_RAMDISK_MB`) is a limit, not an
 * allocation: memory is taken in 32 KB chunks as files are written and
 * given back as they are deleted. Writing more than PSRAM has left fails
 * as a disk error. The contents are gone at reboot.
 *
 * @par Thread Safety
 * `init()` once at boot.
 */
namespace RamDisk {

        /**
         * @brief Boot stage: creates, formats and mounts the RAM disk (nothing
         *        when its configured size is 0).
         * @return `Ok`, or why it couldn't (the system runs on without it).
         */
        Error init();

} // namespace RamDisk

BMDS_NAMESPACE_END
