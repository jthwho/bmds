/**
 * @file      trapentry.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_loader: BMDS's trap entry in front of ESP-IDF's (devplan/protection.md §4.2).
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The trap entry: installed on each core at start-up (an IDF start-up hook, before the
 *        scheduler), `mtvec` and `mtvt` then lead through trapentry.S to IDF's handlers.
 *
 * @par Thread Safety
 * Set before the scheduler starts and only read afterwards.
 */
namespace TrapEntry {

        /** @brief What installing found. */
        enum class State : uint8_t {
                Off,        ///< Not built in (CONFIG_BMDS_TRAP_ENTRY off), or not run on this core.
                Installed,  ///< `mtvec` and `mtvt` lead through BMDS's entry.
                UnknownIdf, ///< BMDS's table doesn't match IDF's (its options changed it): IDF's own kept.
                NotIdfs,    ///< The vectors weren't IDF's when it ran: left alone.
        };

        /** @brief The state on @p core (0 or 1); `Off` for another. */
        State state(unsigned core);

        /** @brief Logs one line unless both cores are `Installed` (once, at boot). */
        void logState();

} // namespace TrapEntry

BMDS_NAMESPACE_END
