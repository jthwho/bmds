/**
 * @file      transfercommands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * File transfer over the console's serial line: `rz` and `sz` (YMODEM, as
 * lrzsz's `sz --ymodem` / `rz --ymodem` on the PC speak it). Pure code over
 * the VFS and the shell's console, so the host tests build them too; they
 * are registered with BMDS_SHELL_COMMAND in transfercommands.cpp.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace TransferCommands {

        int rz(CommandContext &ctx); ///< `rz [-y]`: receive files into the current directory.
        int sz(CommandContext &ctx); ///< `sz file...`: send files.

} // namespace TransferCommands

BMDS_NAMESPACE_END
