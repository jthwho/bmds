/**
 * @file      sysconfhome.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_sysconf: the `home` parameter and what follows it
 * (devplan/sysconf.md §7).
 */

#pragma once

#include <bmds/config.h>
#include <bmds/configtypes.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

namespace SysConf::Home {

        /** @brief Where `/home` leads by default, and while `home` can't be reached. */
        inline constexpr const char *DefaultPath = "/flash/home";

        /** @brief The `home` parameter's declaration (also for host tests). */
        const Config::Param &param();

        /**
         * @brief Makes `/home` follow `home`: now, and at every change. Creates
         *        the default home if it is missing. After the registry is set up.
         */
        Error start();

        /** @brief Stops following `home` (host tests). */
        void stop();

} // namespace SysConf::Home

BMDS_NAMESPACE_END
