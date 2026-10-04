/**
 * @file      textcommands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The text built-ins (`echo`, `cat`, `head`, `wc`, `grep`): pure code that
 * reads stdin and writes stdout, so they are also built for the host tests.
 * They are registered with BMDS_SHELL_COMMAND in textcommands.cpp; the
 * declarations here let the host tests build their own command table.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace TextCommands {

        int cat(CommandContext &ctx);  ///< `cat [-]`: copy stdin to stdout.
        int echo(CommandContext &ctx); ///< `echo [-neE] [word]...`, as bash's.
        int grep(CommandContext &ctx); ///< `grep [-civnqF] pattern`: select lines of stdin.
        int head(CommandContext &ctx); ///< `head [-n count]`: the first lines of stdin.
        int wc(CommandContext &ctx);   ///< `wc [-lwc]`: count lines, words, bytes of stdin.

} // namespace TextCommands

BMDS_NAMESPACE_END
