/**
 * @file      langbuiltins.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The shell language's built-ins (control flow, variables, `test`). They
 * are registered with BMDS_SHELL_COMMAND in langbuiltins.cpp; the
 * declarations here let the host tests build their own command table.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace LangBuiltins {

        int colon(CommandContext &ctx);       ///< `:` / `true`
        int falseCmd(CommandContext &ctx);    ///< `false`
        int test(CommandContext &ctx);        ///< `test expr`, `[ expr ]`
        int breakCmd(CommandContext &ctx);    ///< `break [n]`
        int continueCmd(CommandContext &ctx); ///< `continue [n]`
        int returnCmd(CommandContext &ctx);   ///< `return [n]`
        int exitCmd(CommandContext &ctx);     ///< `exit [n]`
        int eval(CommandContext &ctx);        ///< `eval [word]...`
        int set(CommandContext &ctx);         ///< `set [-+Ceuxo] [--] [arg]...`
        int shift(CommandContext &ctx);       ///< `shift [n]`
        int exportCmd(CommandContext &ctx);   ///< `export [-p] [name[=value]]...`
        int readonly(CommandContext &ctx);    ///< `readonly [-p] [name[=value]]...`
        int unset(CommandContext &ctx);       ///< `unset [-fv] name...`
        int local(CommandContext &ctx);       ///< `local name[=value]...`
        int printfCmd(CommandContext &ctx);   ///< `printf [-v var] format [arg]...`
        int read(CommandContext &ctx);        ///< `read [-r] [-p prompt] [name]...`
        int type(CommandContext &ctx);        ///< `type [-t] name...`
        int command(CommandContext &ctx);     ///< `command [-vV] name [arg]...`
        int env(CommandContext &ctx);         ///< `env [name=value]... [command [arg]...]`
        int term(CommandContext &ctx);        ///< `term [color [depth] | size [COLSxROWS]]`
        int clear(CommandContext &ctx);       ///< `clear`: clears the screen and its scrollback
        int reset(CommandContext &ctx);       ///< `reset`: the terminal and the console's modes back to their start
        int source(CommandContext &ctx);      ///< `source file [arg]...`, `. file [arg]...`
        int sh(CommandContext &ctx);          ///< `sh [-c command [name] | file] [arg]...`
        int jobs(CommandContext &ctx);        ///< `jobs [-l | -p]`
        int wait(CommandContext &ctx);        ///< `wait [%N | pid]...`
        int disown(CommandContext &ctx);      ///< `disown [-h] [-a | %N...]`
        int fg(CommandContext &ctx);          ///< `fg [%N]`
        int bg(CommandContext &ctx);          ///< `bg [%N...]`

} // namespace LangBuiltins

BMDS_NAMESPACE_END
