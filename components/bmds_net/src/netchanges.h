/**
 * @file      netchanges.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * How bmds_net tells NetStatus (netstatus.h) that the interfaces' state may
 * have changed (devplan/network.md §20). Private to bmds_net.
 */

#pragma once

#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace NetStatus {

        /** @brief Something told to watchers may have changed: looked at by `publish()`. Any task. */
        void noteChange();

        /**
         * @brief Network task, core lock held: after `noteChange()`, compares the
         *        interfaces' state with the state last told and tells the watchers if
         *        it differs.
         */
        void publish();

} // namespace NetStatus

BMDS_NAMESPACE_END
