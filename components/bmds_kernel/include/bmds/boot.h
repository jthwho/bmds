/**
 * @file      boot.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The BMDS boot sequence.
 *
 * Runs the init stages (a constexpr table in rodata) in order, timing each,
 * then prints a boot report: versions, chip, stage timings, and per-MemSpace
 * memory usage.
 *
 * @par Thread Safety
 * Called once, from `app_main()`.
 */
class Boot {
        public:
                /**
                 * @brief Runs the boot sequence.
                 *
                 * Returns when all stages have run; services started by the
                 * stages keep running in their own tasks.
                 */
                static void run();
};

BMDS_NAMESPACE_END
