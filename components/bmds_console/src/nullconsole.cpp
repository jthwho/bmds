/**
 * @file      nullconsole.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/nullconsole.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit NullConsole shared;

} // namespace

NullConsole &NullConsole::instance() {
        return shared;
}

BMDS_NAMESPACE_END
