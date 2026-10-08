/**
 * @file      timecommands.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The time built-ins (`date`, `clock`): pure code over the kernel `Clock`,
 * so they are also built for the host tests. They are registered with
 * BMDS_SHELL_COMMAND in timecommands.cpp; the declarations here let the
 * host tests build their own command table.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace TimeCommands {

        /** @brief `date [-u] [+FORMAT] | date [-u] -s DATE`: show or set the time. */
        int date(CommandContext &ctx);

        /**
         * @brief `clock [tzdata | adjust SECONDS]`: the system clock's state; the
         *        zone database. The zone and the database are set by sysconf's
         *        `clock/zone` and `clock/tzdata` (`devplan/sysconf.md` §13).
         */
        int clock(CommandContext &ctx);

} // namespace TimeCommands

BMDS_NAMESPACE_END
