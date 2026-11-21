/**
 * @file      iopad.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A pad's state as read from the hardware (devplan/io.md §3.4), and the
 * words the `io` command and the I/O window show it with. Pure: the
 * register reader is the chip's (<bmds/io.h>, `Io::readPad()`).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/iochip.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Everything about one pad's configuration and level.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPadState {
                uint8_t  function = 0;     ///< The IO_MUX function selected (MCU_SEL).
                uint8_t  drive = 0;        ///< Drive strength, 0 (weakest) to 3.
                uint16_t outputSignal = 0; ///< The matrix output signal on it (`IoChip::gpioOutputSignal`: plain GPIO).
                bool     inputEnabled = false;   ///< Its input buffer is on.
                bool     outputEnabled = false;  ///< Its output driver is on (the GPIO enable bit).
                bool outputByPeripheral = false; ///< The peripheral's signal decides output enable, not the enable bit.
                bool openDrain = false;          ///< Open drain.
                bool pullUp = false;             ///< The weak pull-up is on.
                bool pullDown = false;           ///< The weak pull-down is on.
                bool level = false;              ///< The level read at the pad (meaningful while its input is on).
                bool outputLevel = false;        ///< The GPIO output register's bit for it.
                bool lpIo = false;               ///< Given to the LP IO domain (the LP core or LP peripherals have it).
};

/** @brief Room for `ioRouteText()`'s text, NUL included. */
inline constexpr size_t IoRouteTextBytes = 48;

/**
 * @brief What drives and reads the pad: `GPIO`, an IO_MUX function
 *        (`UART0_TXD (IO_MUX)`), matrix signals (`I2C0_SDA`), or `LP IO`.
 * @param inputs The input signals the matrix routes from this pad.
 * @return The text's length (truncated to fit).
 */
size_t ioRouteText(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs, char *out,
                   size_t size);

/**
 * @brief True if a peripheral switches the pad's output (an IO_MUX peripheral
 *        function, or a matrix signal whose peripheral has the output enable).
 */
bool ioPeripheralDriven(const IoChip &chip, unsigned pad, const IoPadState &state);

/**
 * @brief The pad's direction: `in`, `out`, `in/out`, `periph` (the
 *        peripheral switches its output), or `-` (neither); `,od` after for
 *        open drain.
 */
const char *ioDirectionText(const IoChip &chip, unsigned pad, const IoPadState &state);

/** @brief The pulls: `up`, `down`, `up+down` or `-`. */
const char *ioPullText(const IoPadState &state);

BMDS_NAMESPACE_END
