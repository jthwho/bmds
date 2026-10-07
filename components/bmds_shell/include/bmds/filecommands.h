/**
 * @file      filecommands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The file built-ins: pure code over the VFS, so they are also built for
 * the host tests. They are registered with BMDS_SHELL_COMMAND in
 * filecommands.cpp; the declarations here let the host tests build their
 * own command table.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace FileCommands {

        int cd(CommandContext &ctx);      ///< `cd [dir | -]`: change the shell's current directory.
        int pwd(CommandContext &ctx);     ///< `pwd`: print it.
        int ln(CommandContext &ctx);      ///< `ln -s [-f] target name`, `ln -s [-f] target... dir`
        int ls(CommandContext &ctx);      ///< `ls [-la1] [path]...`: list directories.
        int cp(CommandContext &ctx);      ///< `cp [-r] source... target`: copy.
        int mv(CommandContext &ctx);      ///< `mv source... target`: move or rename.
        int rm(CommandContext &ctx);      ///< `rm [-rf] path...`: remove.
        int trash(CommandContext &ctx);   ///< `trash path... | -l | -r name... | -e [mount]` (devplan/files.md F5).
        int mkdir(CommandContext &ctx);   ///< `mkdir [-p] dir...`: make directories.
        int rmdir(CommandContext &ctx);   ///< `rmdir dir...`: remove empty directories.
        int touch(CommandContext &ctx);   ///< `touch file...`: create, or set the time to now.
        int hexdump(CommandContext &ctx); ///< `hexdump [-C] [file]`: canonical hex and ASCII.

} // namespace FileCommands

BMDS_NAMESPACE_END
