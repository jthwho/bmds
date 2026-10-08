/**
 * @file      fscommands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The filesystem built-ins: pure code over the VFS, so they are also built
 * for the host tests. They are registered with BMDS_SHELL_COMMAND in
 * fscommands.cpp; the declarations here let the host tests build their own
 * command table.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace FsCommands {

        /** @brief `mount [DEVICE PATH]`: list the mounts, or mount a device's filesystem. */
        int mount(CommandContext &ctx);

        /** @brief `umount PATH`: unmount. */
        int umount(CommandContext &ctx);

        /** @brief `sync`: what the filesystems hold written to their devices (`Vfs::sync()`). */
        int sync(CommandContext &ctx);

        /** @brief `df [-h]`: size and free space of the mounts. */
        int df(CommandContext &ctx);

        /** @brief `mkfs [-t auto|fat32|exfat] -y DEVICE`: format, then mount. */
        int mkfs(CommandContext &ctx);

        /**
         * @brief `waitmount PATH MS`: waits up to MS milliseconds for a
         *        filesystem at PATH while mounts are on their way
         *        (`Vfs::isSettling()`: a card being probed); 0 if one is there,
         *        1 if none came or none can (an empty slot), 130 on Ctrl-C
         *        (`devplan/storage.md` §13).
         */
        int waitmount(CommandContext &ctx);

} // namespace FsCommands

BMDS_NAMESPACE_END
