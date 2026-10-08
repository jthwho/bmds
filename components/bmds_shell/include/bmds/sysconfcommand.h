/**
 * @file      sysconfcommand.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The `sysconf` command (`devplan/sysconf.md` §6), also `/bin/sysconf`.
 *
 * @par Thread Safety
 * Any task (the registry is thread-safe).
 */
namespace SysConfCommand {

        /** @brief Runs `sysconf` with @p ctx's arguments. */
        int run(CommandContext &ctx);

} // namespace SysConfCommand

BMDS_NAMESPACE_END
