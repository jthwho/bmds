/**
 * @file      ioport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A port's setup, `io/uartN`'s, `io/spiN`'s and `io/rmtN`'s values (devplan/io.md §11.1, §11.5,
 * §13.1): parsed and checked against the chip.
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

/**
 * @brief An SPI port's setup: `sck=4 mosi=53 miso=54 cs=5 mode=0 clock=1m` (devplan/io.md §11.5).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoSpiSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = IoUartSetup::ReasonBytes;

                static constexpr size_t   MaxCs = 6;           ///< Chip selects (`/dev/spidevB.0` to `.5`).
                static constexpr uint32_t MinClock = 1000;     ///< The slowest `clock=`.
                static constexpr uint32_t MaxClock = 80000000; ///< The fastest `clock=`.

                int8_t   sck = -1;          ///< `sck=`: its pad, or -1.
                int8_t   mosi = -1;         ///< `mosi=`: its pad, or -1.
                int8_t   miso = -1;         ///< `miso=`: its pad, or -1.
                int8_t   cs[MaxCs] = {};    ///< `cs=`: the pads, `csCount` of them.
                uint8_t  csCount = 0;       ///< None: one device with no chip select.
                uint8_t  mode = 0;          ///< `mode=`: 0 to 3 (CPOL × 2 + CPHA).
                uint32_t clock = 1000000;   ///< `clock=`, Hz.
                uint8_t  bits = 8;          ///< `bits=`: 8, 16 or 32.
                bool     lsbFirst = false;  ///< `lsb`.
                bool     csHigh = false;    ///< `cs-high`: chip selects active high.
                bool     threeWire = false; ///< `3wire`: MOSI both ways.
                bool     loopback = false;  ///< `loopback`: MISO read from MOSI inside the chip.
                bool     force = false;     ///< `force`: its pins taken from their holders.

                /**
                 * @brief Parses @p text for @p chip: words known, pads that exist, each once, `sck`
                 *        and `mosi` or `miso`, no `miso` with `3wire` or `loopback`.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief True if @p other sets the port up the same way. */
                bool operator==(const IoSpiSetup &other) const;
};

/**
 * @brief An RMT port's setup: `pin=46 dir=tx resolution=1m continuous` (devplan/io.md §13.1).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoRmtSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = IoUartSetup::ReasonBytes;

                static constexpr uint32_t MinResolution = 10000;    ///< The coarsest `resolution=`, Hz.
                static constexpr uint32_t MaxResolution = 80000000; ///< The finest.
                static constexpr uint32_t MinBuffer = 64;           ///< The smallest `buffer=`, symbols.
                static constexpr uint32_t MaxBuffer = 16384;        ///< The largest.
                static constexpr uint32_t MaxGlitchNs = 3000;       ///< `min=`: the hardware filter's reach.
                static constexpr uint32_t MaxTicks = 32767;         ///< A duration's most ticks (15 bits).

                int8_t   pin = -1;             ///< `pin=`: its pad.
                bool     receive = false;      ///< `dir=rx` (else `dir=tx`).
                uint32_t resolution = 1000000; ///< `resolution=`, ticks a second.
                bool     idleHigh = false;     ///< `idle=high`: a sender rests high.
                bool     invert = false;       ///< `invert`: the pad inverted.
                bool     continuous = false;   ///< `continuous`: a sender fills a dry ring with idle.
                uint32_t buffer = 1024;        ///< `buffer=`: the ring, in symbols.
                uint32_t minNs = 0;            ///< `min=`: a receiver drops shorter pulses (0: none).
                uint32_t maxNs = 0;            ///< `max=`: a level longer ends a run (0: the longest counted).
                bool     monitor = false;      ///< `monitor`: a receiver reads a pad it doesn't take.
                bool     force = false;        ///< `force`: its pin taken from its holder.

                /**
                 * @brief Parses @p text for @p chip: words known, a pad that exists, `dir=`; a
                 *        sender's words not on a receiver and the other way; `max=` within 32767
                 *        ticks of the resolution.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief `max=` in ticks: the longest level a run takes (32767 when not given). */
                uint32_t maxTicks() const;

                /** @brief True if @p other sets the port up the same way. */
                bool operator==(const IoRmtSetup &other) const;
};

BMDS_NAMESPACE_END
