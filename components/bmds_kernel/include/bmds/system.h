/**
 * @file      system.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/** @brief Static facts about the running system. Strings live in flash. */
struct SystemInfo {
                const char *bmdsVersion;  ///< BMDS version, e.g. "0.1.0".
                const char *buildVersion; ///< Application build version (git describe).
                const char *idfVersion;   ///< ESP-IDF version.
                const char *boardName;    ///< Board name from the board definition.
                unsigned    chipRevMajor; ///< Chip revision, major.
                unsigned    chipRevMinor; ///< Chip revision, minor.
                unsigned    cores;        ///< Number of CPU cores.
                unsigned    cpuMhz;       ///< CPU clock in MHz.
};

/**
 * @brief System-wide queries and control.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
class System {
        public:
                /** @brief Returns version, board and chip information. */
                static SystemInfo info();

                /** @brief Returns microseconds since reset. @nonblocking */
                static uint64_t uptimeUs();

                /** @brief Restarts the chip. Flush consoles first. */
                [[noreturn]] static void restart();
};

BMDS_NAMESPACE_END
