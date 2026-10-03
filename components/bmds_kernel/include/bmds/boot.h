/**
 * @file      boot.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The BMDS boot sequence.
 *
 * Initializes logging, runs the kernel's init stages and then the system
 * stages supplied by `main` (consoles, shells, ...), timing each. Then
 * prints a boot report: versions, chip, stage timings, and per-MemSpace
 * memory usage.
 *
 * The kernel doesn't know about consoles or the shell; `main` composes
 * the system by passing their stages here.
 *
 * @par Thread Safety
 * Called once, from `app_main()`.
 */
class Boot {
        public:
                /** @brief One init stage. Stage tables are constexpr (rodata). */
                struct Stage {
                                const char *name; ///< Shown in the boot report.
                                Error (*init)();  ///< Runs the stage; a failure is reported, boot continues.
                };

                /**
                 * @brief Runs the boot sequence.
                 *
                 * Returns when all stages have run; services started by the
                 * stages keep running in their own tasks.
                 *
                 * @param systemStages Stages run after the kernel's own, in order.
                 */
                static void run(Span<const Stage> systemStages);

                /**
                 * @brief Waits until `run()` has finished, boot report included.
                 *
                 * Tasks started by boot stages (shells) call this before their
                 * first console output, so it doesn't land in the middle of the
                 * boot report.
                 * @return `Error::Ok`, `Error::Timeout`, or `Error::InvalidState`
                 *         if `run()` hasn't started.
                 * @blocking
                 */
                static Error waitUntilComplete(unsigned int timeoutMs);
};

BMDS_NAMESPACE_END
