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

} // namespace FsCommands

BMDS_NAMESPACE_END
