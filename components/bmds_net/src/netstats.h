/**
 * @file      netstats.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network's counters in the system's registry (devplan/network.md §21). Private to
 * bmds_net.
 */

#pragma once

#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace NetStats {

        /**
         * @brief Adds net.ip and a net.ethN for each interface made (boot, once, after the
         *        interfaces, without the core lock: a group's read takes it).
         */
        void init();

} // namespace NetStats

BMDS_NAMESPACE_END
