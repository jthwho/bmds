/**
 * @file      tmpfs.h
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
 * @brief `/tmp`: scratch files in memory, without a card (devplan/storage.md §15).
 *
 * A tmpfs (`Vfs::mountTmpfs()`) mounted at `Vfs::TmpPath` at boot. Its size,
 * sysconf's `tmp/size`, is a limit, not an allocation: `Psram` is taken as
 * files are written and given back as they are removed. A change to it applies
 * at once; 0 at boot mounts nothing. The contents are gone at reboot.
 *
 * @par Thread Safety
 * `init()` once at boot.
 */
namespace TmpFs {

        /**
         * @brief Boot stage: mounts `/tmp` (nothing when `tmp/size` is 0); after sysconf.
         * @return `Ok`, or why it couldn't (the system runs on without it).
         */
        Error init();

} // namespace TmpFs

BMDS_NAMESPACE_END
