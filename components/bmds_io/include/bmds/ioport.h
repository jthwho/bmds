/**
 * @file      ioport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A port's setup, `io/uartN`'s value (devplan/io.md §11.1): parsed and checked against the chip.
 * Pure, host-tested; `Io` checks the holders and runs the port.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/iochip.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A UART port's setup: `tx=37 rx=38 baud=115200 format=8n1`.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoUartSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = 160;

                static constexpr uint32_t MinBaud = 300;     ///< The slowest `baud=`.
                static constexpr uint32_t MaxBaud = 5000000; ///< The fastest `baud=`.

                int8_t   tx = -1;          ///< `tx=`: its pad, or -1.
                int8_t   rx = -1;          ///< `rx=`: its pad, or -1.
                int8_t   rts = -1;         ///< `rts=`: its pad, or -1.
                int8_t   cts = -1;         ///< `cts=`: its pad, or -1.
                uint32_t baud = 115200;    ///< `baud=`.
                uint8_t  dataBits = 8;     ///< `format=`'s first: 5 to 8.
                char     parity = 'n';     ///< `format=`'s second: `n`, `e`, `o`.
                uint8_t  stopBits = 1;     ///< `format=`'s third: 1 or 2.
                bool     rtsCts = false;   ///< `flow=rtscts`.
                bool     loopback = false; ///< `loopback`: TX tied to RX inside the UART.
                bool     force = false;    ///< `force`: its pins taken from their holders.

                /**
                 * @brief Parses @p text for @p chip: words known, pads that exist, each once, at
                 *        least one of `tx` and `rx`, `rts`/`cts` with `flow=rtscts`.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief True if @p other sets the port up the same way. */
                bool operator==(const IoUartSetup &other) const;
};

BMDS_NAMESPACE_END
