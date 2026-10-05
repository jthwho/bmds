/**
 * @file      displaytest.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The display's bring-up test picture (`devplan/display.md` §5),
 *        until the text console replaces it.
 */
namespace DisplayTest {

        /**
         * @brief Boot stage: with `CONFIG_BMDS_DISPLAY_TEST_AT_BOOT`, sets the
         *        HDMI bridge up and starts the 1280×720, 60 Hz test picture on
         *        the line-rendered scanout (RGB888, 3 blocks of 8 lines);
         *        otherwise nothing. A board without a display: `Ok`.
         */
        Error init();

} // namespace DisplayTest

BMDS_NAMESPACE_END
