/**
 * @file      iopad.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The words a pad's state is shown with (devplan/io.md §3.4).
 */

#include <cstring>
#include <bmds/iopad.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Appends @p text to @p out (of @p size, at @p length so far), truncating.
        size_t append(char *out, size_t size, size_t length, const char *text) {
                if (size == 0) return 0;
                while (*text != '\0' && length + 1 < size) out[length++] = *text++;
                out[length] = '\0';
                return length;
        }

        bool isGpio(const IoChip &chip, const IoPadState &state) {
                return state.function == chip.gpioFunction;
        }

} // namespace

size_t ioRouteText(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs, char *out,
                   size_t size) {
        if (out == nullptr || size == 0) return 0;
        out[0] = '\0';
        if (state.lpIo) return append(out, size, 0, "LP IO");
        if (!isGpio(chip, state)) {
                const IoMuxFunction *f = chip.function(pad, state.function);
                // The pad's own function (its reset value) with nothing on: idle.
                if (f != nullptr && !f->peripheral && !state.inputEnabled && !state.outputEnabled)
                        return append(out, size, 0, "-");
                size_t n = append(out, size, 0, f != nullptr ? f->name : "?");
                return append(out, size, n, " (IO_MUX)");
        }
        size_t      n = 0;
        const char *output = nullptr;
        // The output's signal counts when something drives the pad: its enable bit, or the
        // peripheral's own enable.
        if (state.outputSignal != chip.gpioOutputSignal)
                output = chip.outputName(state.outputSignal);
        else if (state.outputEnabled)
                output = "GPIO";
        if (output != nullptr) n = append(out, size, n, output);
        for (uint16_t signal : inputs) {
                const char *name = chip.inputName(signal);
                if (name == nullptr) continue;
                // A bidirectional signal (I2C0_SDA) is named once.
                if (output != nullptr && std::strcmp(name, output) == 0) continue;
                if (n > 0) n = append(out, size, n, ",");
                n = append(out, size, n, name);
        }
        if (n == 0) n = append(out, size, n, state.inputEnabled ? "GPIO" : "-");
        return n;
}

const char *ioDirectionText(const IoChip &chip, unsigned pad, const IoPadState &state) {
        // [mode][open drain]
        static constexpr const char *Texts[5][2] = {
                {"-", "-,od"}, {"in", "in,od"}, {"out", "out,od"}, {"in/out", "in/out,od"}, {"periph", "periph,od"},
        };
        size_t mode = 0;
        // An IO_MUX peripheral function drives the output enable itself; so does a matrix signal
        // whose peripheral has the enable.
        const IoMuxFunction *f = isGpio(chip, state) ? nullptr : chip.function(pad, state.function);
        const bool peripheral = (f != nullptr && f->peripheral) || (isGpio(chip, state) && state.outputByPeripheral &&
                                                                    state.outputSignal != chip.gpioOutputSignal);
        if (peripheral && !state.lpIo)
                mode = 4;
        else if (state.inputEnabled && state.outputEnabled)
                mode = 3;
        else if (state.outputEnabled)
                mode = 2;
        else if (state.inputEnabled)
                mode = 1;
        return Texts[mode][state.openDrain ? 1 : 0];
}

const char *ioPullText(const IoPadState &state) {
        if (state.pullUp && state.pullDown) return "up+down";
        if (state.pullUp) return "up";
        if (state.pullDown) return "down";
        return "-";
}

BMDS_NAMESPACE_END
