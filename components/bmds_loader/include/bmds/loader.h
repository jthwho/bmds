/**
 * @file      loader.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/programlauncher.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Runs BMDS programs from files (devplan/programs.md): checks the
 *        file (`ProgramImage`), loads it into PSRAM at a 4096-byte-aligned
 *        address, relocates it, syncs the caches and runs it in a task of its
 *        own, with the caller's file descriptors, arguments and environment
 *        (`Process`), then frees everything it had.
 *
 * Every program is a process (`ProcessTable`); programs start others with
 * vfork and exec (devplan/programs.md §13). A program the shell starts is
 * a job: a second Ctrl-C kills it and every process it started.
 *
 * Target only. The object is stateless: one serves every shell, and
 * several programs may run at once (pipeline stages). The processes are
 * the system's, set up once by `init()`.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class Loader final : public ProgramLauncher {
        public:
                constexpr Loader() = default;

                /** @brief Sets up the process table (a boot stage). */
                static Error init();

                /** @copydoc ProgramLauncher::run */
                Result<int> run(const Request &request) override;
};

BMDS_NAMESPACE_END
