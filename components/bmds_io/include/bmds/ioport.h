/**
 * @file      ioport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A port's setup, `io/uartN`'s, `io/spiN`'s, `io/rmtN`'s, `io/pwmN`'s, `io/pcntN`'s and
 * `io/parlioN`'s values (devplan/io.md §11.1, §11.5, §13.1, §15.1, §15.3, §15.5): parsed and
 * checked against the chip.
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
                bool     dma = false;       ///< `dma`: transfers by GDMA in 4 KB pieces (else the 64-byte FIFO).
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
                static constexpr uint32_t MaxPulseUs = 100000;      ///< `pulse-width=`'s longest.
                static constexpr uint32_t DefaultDma = 1024;        ///< `dma`'s buffer, symbols.
                static constexpr uint32_t MinDma = 64;              ///< `dma=`'s smallest.
                static constexpr uint32_t MaxDma = 2046;            ///< Its largest (two DMA nodes of 4095 bytes).

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
                int8_t   pulsePin = -1;        ///< `pulse=`: a sender's pad pulsed at each mark, or -1.
                uint32_t pulseWidthUs = 10;    ///< `pulse-width=`, µs.
                bool     pulseLow = false;     ///< `pulse-low`: the pulse goes low (it rests high).
                uint32_t dma = 0;              ///< `dma[=SYMBOLS]`: a sender's DMA buffer (internal RAM), or 0: none.
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

/**
 * @brief A PWM channel's setup: `pin=21 period=1ms duty=25% offset=0` (devplan/io.md §15.1).
 *
 * Times take ns, us, ms or s (decimals allowed; a bare number is ns); duty and offset also a share
 * of the period (`25%`, to hundredths).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPwmSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = IoUartSetup::ReasonBytes;

                static constexpr uint64_t MaxPeriodNs =
                        60000000000ull;                  ///< `period=`'s longest (the hardware's: 13.4 s).
                static constexpr uint32_t Whole = 10000; ///< A share's whole: 100.00 %.

                int8_t   pin = -1;            ///< `pin=`: its pad.
                uint64_t periodNs = 0;        ///< `period=`: 0, off.
                uint64_t duty = 0;            ///< `duty=`: ns, or hundredths of a percent (`dutyShare`).
                bool     dutyShare = false;   ///< `duty=` was a share of the period.
                uint64_t offset = 0;          ///< `offset=`: ns, or hundredths of a percent (`offsetShare`).
                bool     offsetShare = false; ///< `offset=` was a share of the period.
                bool     force = false;       ///< `force`: its pin taken from its holder.

                /**
                 * @brief Parses @p text for @p chip: words known, a pad that exists, a duty no longer
                 *        than the period and an offset shorter than it.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief The duty in ns (a share resolved against the period). */
                uint64_t dutyNs() const { return dutyShare ? periodNs * duty / Whole : duty; }

                /** @brief The offset in ns. */
                uint64_t offsetNs() const { return offsetShare ? periodNs * offset / Whole : offset; }

                /** @brief True if @p other sets the channel up the same way. */
                bool operator==(const IoPwmSetup &other) const;
};

/**
 * @brief A pulse counter's setup: `a=46 b=47 function=quadrature-x4` (devplan/io.md §15.3).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPcntSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = IoUartSetup::ReasonBytes;

                static constexpr uint32_t MaxFilterNs = 10000; ///< `filter=`'s longest (1023 APB cycles bound it too).

                /** @brief How the signals count: Linux's `enum counter_function`, in its order. */
                enum class Function : uint8_t {
                        Increase,       ///< A's edges (`action=`) count up.
                        Decrease,       ///< A's edges count down.
                        PulseDirection, ///< A's rising edges, up while B is high, down while low.
                        QuadratureX1A,  ///< A's rising edges, the direction from B.
                        QuadratureX1B,  ///< B's rising edges, the direction from A.
                        QuadratureX2A,  ///< A's both edges.
                        QuadratureX2B,  ///< B's both edges.
                        QuadratureX4,   ///< Every edge of both.
                };

                /** @brief Which of A's edges count for `increase` and `decrease`: Linux's synapse actions. */
                enum class Action : uint8_t {
                        None,
                        Rising,
                        Falling,
                        Both
                };

                int8_t   a = -1;                        ///< `a=`: signal A's pad.
                int8_t   b = -1;                        ///< `b=`: signal B's pad, or -1.
                Function function = Function::Increase; ///< `function=`.
                Action   action = Action::Rising;       ///< `action=`: `increase` and `decrease` only.
                uint32_t filterNs = 0;                  ///< `filter=`: shorter pulses ignored, ns (0: none).
                bool     pullUp = false;                ///< `pull-up` on the pads.
                bool     pullDown = false;              ///< `pull-down`.
                bool     monitor = false;               ///< `monitor`: pads read, not taken.
                bool     force = false;                 ///< `force`: its pins taken from their holders.

                /**
                 * @brief Parses @p text for @p chip: words known, pads that exist, `b=` exactly when
                 *        the function reads B, `action=` only for `increase` and `decrease`.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief True if the function reads signal B. */
                bool readsB() const { return function != Function::Increase && function != Function::Decrease; }

                /** @brief Linux's name for @p f (`quadrature-x4`). */
                static const char *functionName(Function f);

                /** @brief True if @p other sets the counter up the same way. */
                bool operator==(const IoPcntSetup &other) const;
};

/**
 * @brief A PARLIO port's setup: `dir=tx data=46,47,48,16 clock=10m clk=17` (devplan/io.md §15.5).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoParlioSetup {
                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = IoUartSetup::ReasonBytes;

                static constexpr size_t   MaxData = 16;        ///< Data lines at most.
                static constexpr uint32_t MinClock = 1000;     ///< The slowest `clock=`.
                static constexpr uint32_t MaxClock = 80000000; ///< The fastest (the hardware may refuse less).
                static constexpr uint32_t MinBuffer = 4096;    ///< A receiver's smallest `buffer=`.
                static constexpr uint32_t MaxBuffer =
                        65472; ///< Its largest (one frame of the hardware's, whole cache lines).
                static constexpr uint32_t DefaultBuffer = 16384; ///< Unless given.

                bool     receive = false;    ///< `dir=rx` (else `dir=tx`).
                int8_t   data[MaxData] = {}; ///< `data=`: the pads, `dataCount` of them, line 0 first.
                uint8_t  dataCount = 0;      ///< How many.
                uint32_t clock = 1000000;    ///< `clock=`: the rate, Hz (an external clock's expected rate).
                int8_t   clk = -1;           ///< `clk=`: the clock out's pad, or -1.
                int8_t   clkIn = -1;         ///< `clk-in=`: an external clock's pad, or -1 (the internal one).
                bool     rising =
                        false; ///< `edge=`: data taken on the rising edge (else, the default, falling: ESP-IDF's).
                bool     msb = false;            ///< `msb`: bits in a byte from the top (width < 8).
                uint32_t idle = 0;               ///< `idle=`: a sender's lines between transfers.
                uint32_t buffer = DefaultBuffer; ///< `buffer=`: a receiver's ring, bytes.
                bool     monitor = false;        ///< `monitor`: a receiver's pads read, not taken.
                uint8_t  drive = 2;              ///< `drive=`: the outputs' strength, 0 to 3 (ESP-IDF's 2).
                bool     force = false;          ///< `force`: its pins taken from their holders.

                /**
                 * @brief Parses @p text for @p chip: words known, pads that exist, each once, `dir=`
                 *        and `data=`; a sender's words not on a receiver and the other way.
                 * @param[out] why Why it is refused, for people, or "".
                 * @return `Ok`, or `Invalid`.
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief The width: 1, 2, 4, 8 or 16 lines, the pins rounded up. */
                uint8_t width() const;

                /** @brief True if @p other sets the port up the same way. */
                bool operator==(const IoParlioSetup &other) const;
};

BMDS_NAMESPACE_END
