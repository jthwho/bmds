/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/boot.h>

/**
 * @brief ESP-IDF application entry point.
 *
 * Hands off to the BMDS boot sequence. When it returns, the IDF main task
 * exits and its stack is reclaimed; BMDS services run in their own tasks.
 */
extern "C" void app_main(void) {
        bmds::Boot::run();
        return;
}
