/**
 * @file      regtable.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A peripheral's registers by name, generated from ESP-IDF's register headers
 * (tools/gen-regtable.py) for `display dsi` (devplan/display.md §5e).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace detail {

        /** @brief A register's field. */
        struct RegField {
                        const char *name;
                        uint8_t     lo;     ///< Its lowest bit.
                        uint8_t     width;  ///< Its bits.
                        uint8_t     access; ///< 0 read-only, 1 read/write, 2 write-to-trigger, 3 other.
        };

        /** @brief A register: its fields are `fields[first, first + count)` of its table. */
        struct RegInfo {
                        const char *name;
                        uint16_t    offset; ///< From the block's base, in bytes.
                        uint16_t    first;
                        uint8_t     count;
                        bool        noRead; ///< Reading it changes it (clear-on-read, a FIFO).
        };

        /** @brief A block's registers. */
        struct RegTable {
                        const RegInfo  *regs;
                        size_t          count;
                        const RegField *fields;
        };

        extern const RegTable DsiHostRegs; // the DSI host (DesignWare)
        extern const RegTable DsiBrgRegs;  // the DSI bridge (DMA to DPI)

} // namespace detail

BMDS_NAMESPACE_END
