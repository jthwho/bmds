/**
 * @file      system.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/** @brief Static facts about the running system. Strings live in flash. */
struct SystemInfo {
                const char *bmdsVersion;  ///< BMDS version, e.g. "0.1.0".
                const char    *buildVersion; ///< Application build version (`PROJECT_VER`: "0.1.0+72cba49a[-dirty]").
                const uint8_t *imageSha256;  ///< The running image's ELF SHA-256 (32 bytes): which build exactly.
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

                /** @brief The longest serial number, in bytes. */
                static constexpr size_t MaxSerialBytes = 31;

                /**
                 * @brief Returns the board's serial number, as its definition reads it
                 *        (`Board::serial`; this board's: the chip's factory MAC, 12 hex
                 *        digits; `devplan/notifications.md` §2), or "" if it has none. Read
                 *        once; thread-safe.
                 */
                static const char *serial();

                /** @brief Returns microseconds since reset. @nonblocking */
                static uint64_t uptimeUs();

                /** @brief Restarts the chip. Flush consoles first. */
                [[noreturn]] static void restart();

                /**
                 * @brief Panics with @p message: the panic's report on the console and in the
                 *        log (devplan/logging.md §9), then a reset counted as a crash. For tests.
                 */
                [[noreturn]] static void panic(const char *message);

                /** @brief Why the chip last reset: "power-on", "panic", "software (reboot)"... */
                static const char *resetReason();

                /** @brief A firmware in the flash (devplan/update.md §4). */
                enum class Firmware : uint8_t {
                        Main,     ///< The main firmware, in `ota_0`.
                        Recovery, ///< The recovery firmware, in `ota_1`: applies updates.
                };

                /** @brief Returns the firmware running. */
                static Firmware running();

                /**
                 * @brief Makes @p firmware the one the next reset starts (`otadata`), once its image
                 *        checks as the bootloader would check it (its hash, a few hundred ms).
                 * @return `Ok`; `NotExist` (no such partition); `CorruptData` (no good image there:
                 *         nothing changed); `IoError`.
                 */
                static Error setNextFirmware(Firmware firmware);
};

BMDS_NAMESPACE_END
