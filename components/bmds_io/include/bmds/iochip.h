/**
 * @file      iochip.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a chip's I/O is (devplan/io.md §3.1): its pads, each pad's IO_MUX
 * functions, the GPIO matrix's signals, and the peripheral instances and
 * channels that pads are claimed with. One `constexpr IoChip` per chip in
 * rodata, generated from ESP-IDF's `soc` headers by tools/gen-iochip.py
 * (`chips/<target>/iochipdef.cpp`); portable code reads only this.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A kind of I/O resource: a pad, or a peripheral instance or channel
 *        a driver claims with its pads (devplan/io.md §3.2).
 *
 * The superset of every chip's kinds; a chip lists those it has
 * (`IoChip::kinds`). Not an ABI: the order may grow anywhere.
 */
enum class IoKind : uint8_t {
        Pad,           ///< A GPIO pad: `GPIOn`.
        Uart,          ///< An HP UART: `uartN`.
        LpUart,        ///< An LP UART: `lp-uartN`.
        I2c,           ///< An HP I²C controller: `i2cN`.
        LpI2c,         ///< An LP I²C controller: `lp-i2cN`.
        Spi,           ///< A general-purpose SPI controller: `spiN` (numbered as the chip does: SPI2...).
        I2s,           ///< An I²S controller: `i2sN`.
        Rmt,           ///< An RMT channel: `rmtN`.
        Ledc,          ///< An LEDC channel: `ledcN`.
        LedcTimer,     ///< An LEDC timer: `ledc-timerN`.
        Pcnt,          ///< A pulse counter unit: `pcntN`.
        Mcpwm,         ///< An MCPWM group: `mcpwmN`.
        Twai,          ///< A TWAI (CAN) controller: `twaiN`.
        Sdmmc,         ///< An SD/MMC host slot: `sdmmcN`.
        Emac,          ///< An Ethernet MAC: `emacN`.
        Uhci,          ///< A UART DMA engine: `uhciN`.
        Parlio,        ///< A parallel I/O unit: `parlioN`.
        UsbSerialJtag, ///< The USB-Serial/JTAG controller: `usb-serial-jtagN`.
        EtmChannel,    ///< An event task matrix channel: `etmN`.
};

/** @brief A kind's instances on a chip (`IoChip::kinds`). */
struct IoKindInfo {
                IoKind      kind = IoKind::Pad; ///< The kind.
                const char *name = "";          ///< Its lower-case name, as in `uart0` (`uart`).
                uint8_t     first = 0;          ///< The first instance's number (SPI2: 2).
                uint8_t     count = 0;          ///< How many.
};

/** @brief One of a pad's IO_MUX functions (`IoChip::functions`). */
struct IoMuxFunction {
                uint8_t     index = 0;          ///< Its MCU_SEL value.
                const char *name = "";          ///< Its signal (`UART0_TXD`), or `GPIO` for the plain GPIO function.
                bool        peripheral = false; ///< A peripheral's signal, which drives the pad's output enable.
};

/** @brief A pad (`IoChip::pads`, indexed by GPIO number). */
struct IoPadInfo {
                /** @brief Facts about the pad. */
                enum Flag : uint8_t {
                        Strapping = 1u << 0, ///< Sampled at reset to choose the boot mode.
                        InputOnly = 1u << 1, ///< Has no output driver.
                };

                uint8_t     flags = 0;         ///< `Flag` bits.
                int8_t      lpIo = -1;         ///< Its LP IO number, or -1 if it has none.
                uint8_t     functionFirst = 0; ///< Its first entry in `IoChip::functions`.
                uint8_t     functionCount = 0; ///< Its IO_MUX functions.
                const char *analog = "";       ///< Its analog functions (`ADC1_CH4`), or "".
                const char *note = "";         ///< The chip's note on it, or "".

                /** @brief True if `flag` is set. */
                constexpr bool is(Flag flag) const { return (flags & flag) != 0; }
};

/**
 * @brief A resource: an index into the chip's flat list of resources (pads
 *        first, numbered as GPIOs, then each kind's instances in turn).
 */
using IoResource = uint16_t;

/** @brief No resource. */
inline constexpr IoResource NoIoResource = 0xffff;

/**
 * @brief A chip's I/O, as rodata.
 *
 * @par Thread Safety
 * Immutable: thread-safe.
 */
struct IoChip {
                const char           *name = "";            ///< The chip (`ESP32-P4`).
                uint8_t               gpioFunction = 0;     ///< The IO_MUX function that is the plain GPIO.
                uint16_t              gpioOutputSignal = 0; ///< The matrix output signal that is the plain GPIO output.
                uint8_t               constZeroInput = 0;   ///< The input selector value for a constant 0.
                uint8_t               constOneInput = 0;    ///< The input selector value for a constant 1.
                Span<const IoPadInfo> pads;                 ///< Indexed by GPIO number.
                Span<const IoMuxFunction> functions;        ///< The pads' IO_MUX functions, pad by pad.
                Span<const char *const>   inputSignals;     ///< Indexed by input signal; nullptr where none.
                Span<const char *const>   outputSignals;    ///< Indexed by output signal; nullptr where none.
                Span<const uint8_t>       inputGroups;      ///< Each input signal's group (`groups`).
                Span<const uint8_t>       outputGroups;     ///< Each output signal's group (`groups`).
                Span<const char *const>   groups;           ///< The signals' peripheral groups (`UART0`, `SPI2`...).
                Span<const IoKindInfo>    kinds;            ///< The resource kinds after the pads, in resource order.

                /** @brief The number of pads. */
                constexpr size_t padCount() const { return pads.size(); }

                /** @brief The pad's IO_MUX functions. */
                constexpr Span<const IoMuxFunction> padFunctions(size_t pad) const {
                        return pad < pads.size() ? functions.mid(pads[pad].functionFirst, pads[pad].functionCount)
                                                 : Span<const IoMuxFunction>();
                }

                /** @brief IO_MUX function @p index of @p pad, or nullptr. */
                const IoMuxFunction *function(size_t pad, uint8_t index) const;

                /** @brief The name of IO_MUX function @p index of @p pad, or nullptr. */
                const char *functionName(size_t pad, uint8_t index) const;

                /** @brief The input signal's name, or nullptr. */
                constexpr const char *inputName(size_t signal) const {
                        return signal < inputSignals.size() ? inputSignals[signal] : nullptr;
                }

                /** @brief The output signal's name, or nullptr. */
                constexpr const char *outputName(size_t signal) const {
                        return signal < outputSignals.size() ? outputSignals[signal] : nullptr;
                }

                /** @brief How many resources there are: the pads and every kind's instances. */
                size_t resourceCount() const;

                /** @brief The resource for instance @p number of @p kind (a pad: its GPIO number). */
                Result<IoResource> resource(IoKind kind, unsigned number) const;

                /** @brief The kind and instance number of @p resource. */
                Result<IoKindInfo> kindOf(IoResource resource, unsigned *number) const;

                /**
                 * @brief Writes the resource's name (`GPIO37`, `uart1`, `spi2`) into
                 *        @p out, NUL ended.
                 * @return Its length, or 0 for no such resource.
                 */
                size_t resourceName(IoResource resource, char *out, size_t size) const;

                /**
                 * @brief Finds a resource by name, as `resourceName()` writes it
                 *        (and `gpioN`, any case).
                 * @return It, or `NotExist`.
                 */
                Result<IoResource> findResource(StringView text) const;

                /** @brief The input signal named @p text (any case), or `NotExist`. */
                Result<uint16_t> findInput(StringView text) const;

                /** @brief The output signal named @p text (any case), or `NotExist`. */
                Result<uint16_t> findOutput(StringView text) const;
};

/**
 * @brief The chip this firmware runs on (the target's generated table).
 * @nonblocking
 */
const IoChip &ioChip();

BMDS_NAMESPACE_END
