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
 * Target only. Stateless: one object serves every shell, and several
 * programs may run at once (pipeline stages).
 *
 * @par Thread Safety
 * Thread-safe.
 */
class Loader final : public ProgramLauncher {
        public:
                constexpr Loader() = default;

                /** @copydoc ProgramLauncher::run */
                Result<int> run(const Request &request) override;
};

BMDS_NAMESPACE_END
