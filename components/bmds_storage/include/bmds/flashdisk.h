/**
 * @file      flashdisk.h
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
 * @brief `/flash`: files that persist on the board's flash (`devplan/storage.md` §13,
 *        `devplan/cfg.md`): the configuration used without a card (`/flash/cfg`), the home
 *        used without one (`/flash/home`), and the update's records (`/flash/update`).
 *
 * The flash partition named `flash` is the block device `flash0`, holding littlefs
 * (`devplan/storage.md` §13d) through an ordered write cache, and mounted at `/flash` for good
 * (it can't be unmounted). On the first boot, a partition without littlefs is formatted. The
 * repository's `flash/` files outside `cfg/` are installed on it (`Cfg` installs the others in
 * `/cfg`).
 *
 * @par Thread Safety
 * `init()` once at boot.
 */
namespace FlashDisk {

        /** @brief Where it is mounted. */
        inline constexpr const char *Path = "/flash";

        /**
         * @brief Boot stage: mounts `/flash`, formatting it on the first boot.
         * @return `Ok`, or why it couldn't (the system runs on without it).
         */
        Error init();

} // namespace FlashDisk

BMDS_NAMESPACE_END
