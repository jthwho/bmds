/**
 * @file      bootdisk.h
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
 * @brief `/boot`: files and settings that persist on the board's flash
 *        (`devplan/storage.md` §13).
 *
 * The flash partition named `boot` is the block device `flash0`, holding
 * littlefs (`devplan/storage.md` §13d) through an ordered write cache, and
 * mounted at `/boot` for good (it can't be unmounted). On the first boot, a
 * partition without littlefs is formatted and given the defaults
 * (`/boot/profile`, read by every interactive shell, and `/boot/autoexec.sh`,
 * run once at startup).
 *
 * @par Thread Safety
 * `init()` once at boot.
 */
namespace BootDisk {

        /** @brief Where it is mounted. */
        inline constexpr const char *Path = "/boot";

        /**
         * @brief Boot stage: mounts `/boot`, formatting it on the first boot.
         * @return `Ok`, or why it couldn't (the system runs on without it).
         */
        Error init();

        /**
         * @brief Writes a new `/boot/random-seed` from the random pool
         *        (devplan/random.md): by the storage service a few seconds
         *        after boot, and at shutdown. Does nothing without `/boot`.
         */
        void refreshRandomSeed();

} // namespace BootDisk

BMDS_NAMESPACE_END
