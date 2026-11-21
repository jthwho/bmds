/**
 * @file      iosetup.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The pins the user sets up, `io/gpio`'s value (devplan/io.md §10.1): parsed and checked against
 * the chip. Pure, host-tested; `Io` checks the holders and applies it.
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
 * @brief One pad's setup: an entry of `io/gpio` (`14=out,high`).
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPinSetup {
                /** @brief Input signals one pad may feed (`in:SIGNAL`), at most. */
                static constexpr size_t MaxInputs = 4;

                /** @brief No matrix output signal: the plain GPIO's. */
                static constexpr uint16_t NoSignal = 0xffff;

                /** @brief The edges watched (devplan/io.md §10.5): bits. */
                enum Edge : uint8_t {
                        NoEdges = 0,
                        Rising = 1u << 0,  ///< `rising` (`edges`: both).
                        Falling = 1u << 1, ///< `falling` (`edges`: both).
                };

                /** @brief The longest `debounce=`, µs. */
                static constexpr uint32_t MaxDebounceUs = 1000000;

                /** @brief Its direction. */
                enum class Mode : uint8_t {
                        Input,     ///< `in`: GPIO input (the default).
                        Output,    ///< `out`: push-pull.
                        OpenDrain, ///< `od`: open drain, its input on.
                        Off,       ///< `off`: neither.
                };

                uint8_t  pad = 0;                ///< Its GPIO number.
                Mode     mode = Mode::Input;     ///< Its direction.
                bool     high = false;           ///< An output's level.
                bool     pullUp = false;         ///< `pull-up`.
                bool     pullDown = false;       ///< `pull-down`.
                bool     force = false;          ///< `force`: taken from its holder.
                uint8_t  drive = 2;              ///< `drive=N`, 0-3.
                int16_t  function = -1;          ///< `func=`: an IO_MUX function, or -1 (the GPIO).
                uint16_t outSignal = NoSignal;   ///< `out:SIGNAL`, or `NoSignal`.
                uint16_t inputs[MaxInputs] = {}; ///< `in:SIGNAL`s.
                uint8_t  inputCount = 0;         ///< How many.
                uint8_t  edges = NoEdges;        ///< `edges`, `rising`, `falling`: `Edge` bits.
                uint32_t debounceUs = 0;         ///< `debounce=US`: an edge this soon after the last is dropped.

                /** @brief True if it drives the pad (out, od, or an IO_MUX function's own choice). */
                bool isOutput() const { return mode == Mode::Output || mode == Mode::OpenDrain; }

                /**
                 * @brief True if its input is on: `in`, `od`, an input signal, an IO_MUX function
                 *        (`IoChipOps::configurePad()`'s rule).
                 */
                bool readsInput() const {
                        return inputCount > 0 ||
                               (mode != Mode::Off && (mode == Mode::Input || mode == Mode::OpenDrain || function >= 0));
                }

                /** @brief True if @p other sets the pad up the same way (`force` included). */
                bool operator==(const IoPinSetup &other) const;
};

/**
 * @brief `io/gpio`'s entries, in the order given.
 *
 * @par Thread Safety
 * Not thread-safe.
 */
class IoGpioSetup {
        public:
                /** @brief Most entries (a chip's pads). */
                static constexpr size_t MaxPins = 64;

                /** @brief Room for `parse()`'s reason, NUL included. */
                static constexpr size_t ReasonBytes = 160;

                /**
                 * @brief Parses @p text for @p chip: words known, pads, functions and signals
                 *        that exist, each pad once, an input signal read by one pad only.
                 * @param[out] why Why it is refused, for people (`GPIO99 doesn't exist`), or "".
                 * @return `Ok`, `Invalid` (a word, a pad, a function, a signal), `Exists` (a
                 *         pad twice, an input signal twice), `Full` (too many).
                 */
                Error parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]);

                /** @brief Returns the entries. */
                size_t count() const { return _count; }

                /** @brief Returns entry @p index. */
                const IoPinSetup &at(size_t index) const { return _pins[index]; }

                /** @brief Returns pad @p pad's entry, or null. */
                const IoPinSetup *find(unsigned pad) const;

        private:
                IoPinSetup _pins[MaxPins];
                size_t     _count = 0;
};

BMDS_NAMESPACE_END
