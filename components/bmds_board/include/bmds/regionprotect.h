/**
 * @file      regionprotect.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The CPU's memory protection, set by BMDS instead of ESP-IDF (devplan/protection.md §4.1):
 * the PMA holds the address space's holes and the kernel's W^X for every privilege mode; the
 * PMP is left empty and unlocked, for granting U-mode programs their memory.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What the start-up setup found and did on one core.
 *
 * Filled once per core in ESP-IDF's secondary start-up stage, before the scheduler, and never
 * changed afterwards.
 */
struct RegionProtection {
                bool     done = false;       ///< The setup ran on this core.
                bool     idfLayout = false;  ///< An older bootloader locked IDF's layout: IDF's setup ran.
                uint16_t pmaMismatch = 0;    ///< PMA entries that didn't take (bit per entry): locked earlier.
                uint16_t pmpLocked = 0;      ///< PMP entries locked before us (bit per entry): no U-mode.
                bool     flashSplit = false; ///< Flash text R X, the rest of the flash window R (else both R X).
                bool     iramSplit = false;  ///< IRAM text R X, the rest of internal RAM R W (else both R W X).
                uint32_t iramTextEnd = 0;    ///< Where IRAM text's entry ends, as it reads back.

                /** @brief The kernel's W^X and holes are in place as designed. */
                bool isPmaSet() const { return done && !idfLayout && pmaMismatch == 0; }

                /** @brief Every PMP entry is free, so programs could be given U-mode. */
                bool isPmpFree() const { return done && !idfLayout && pmpLocked == 0; }
};

/**
 * @brief The setup's result for @p core (0 or 1); a core out of range reads as not done.
 *
 * Thread-safe: the records are written before the scheduler starts and only read afterwards.
 */
const RegionProtection &regionProtection(unsigned core);

BMDS_NAMESPACE_END
