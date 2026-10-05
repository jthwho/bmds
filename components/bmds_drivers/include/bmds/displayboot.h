/**
 * @file      displayboot.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/** @brief The display at boot (`devplan/display.md` §5c). */
namespace DisplayBoot {

        /**
         * @brief Boot stage: with `CONFIG_BMDS_DISPLAY_AT_BOOT`, shows display 0
         *        at 1280×720, 60 Hz, surface 0 single-buffered with the test
         *        picture; otherwise nothing. A board without a display: `Ok`.
         */
        Error init();

} // namespace DisplayBoot

BMDS_NAMESPACE_END
