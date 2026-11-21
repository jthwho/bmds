/**
 * @file      io.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's I/O (devplan/io.md): the one registry of who holds each pad
 * and peripheral instance, the board's wiring in it, and the pads' state
 * read from the hardware. Drivers claim what they use through `Io::claim()`
 * before configuring it:
 *
 * @code
 * const IoUse uses[] = {{Io::pad(b.sda), "SDA"}, {Io::pad(b.scl), "SCL"}, {Io::instance(IoKind::I2c, b.port), ""}};
 * auto [claim, err] = Io::claim("i2c0", uses, 0, nullptr);
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/iochip.h>
#include <bmds/iopad.h>
#include <bmds/iopin.h>
#include <bmds/ioregistry.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace Io {

        /**
         * @brief The boot stage: makes the registry and records the board's pads.
         *        Before it, `claim()` returns `NotReady`.
         * @return `Ok`, or `NoMem`.
         */
        Error init();

        /** @brief True once `init()` has run. @nonblocking */
        bool isReady();

        /** @brief The system's registry. `isReady()` must be true. */
        IoRegistry &registry();

        /** @brief The resource for GPIO @p gpio (a pad's resource is its number). @nonblocking */
        constexpr IoResource pad(int gpio) {
                return gpio < 0 ? NoIoResource : static_cast<IoResource>(gpio);
        }

        /**
         * @brief The resource for instance @p number of @p kind, or
         *        `NoIoResource` if the chip hasn't it.
         * @nonblocking
         */
        IoResource instance(IoKind kind, unsigned number);

        /**
         * @brief A system owner's claim (a driver's): `IoRegistry::claim()` for
         *        `IoOwnerKind::System`. Uses whose resource is `NoIoResource`
         *        (a signal the board hasn't) are left out.
         * @return The claim; `NotReady` before `init()`; or the registry's errors.
         * @blocking Waits for the registry's lock.
         */
        Result<IoClaim> claim(StringView owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked);

        /**
         * @brief Writes why @p resource can't be taken (`GPIO 35 is a system pin:
         *        eth0 uses it as RMII TXD1.`), for the messages of devplan/io.md §3.3.
         * @return The text's length.
         * @blocking Waits for the registry's lock.
         */
        size_t busyText(IoResource resource, char *out, size_t size);

        /**
         * @brief Reads pad @p pad's state from the hardware.
         * @return `Ok`, or `Invalid` (not a pad).
         * @nonblocking
         */
        Error readPad(unsigned pad, IoPadState &out);

        /**
         * @brief Where every input signal comes from through the GPIO matrix
         *        (`IoChipOps::inputSource()`), into @p out (one entry per input
         *        signal, `ioChip().inputSignals.size()` of them; fewer are filled
         *        if @p out is shorter).
         * @nonblocking
         */
        void readInputSources(Span<int16_t> out);

        /**
         * @brief What the board says of pad @p pad: its label and note
         *        (`BoardPin`), and the first connector pin it is brought out to.
         * @nonblocking
         */
        IoPinBoard boardOf(unsigned pad);

        /**
         * @brief Pads' rows (`ioPinRow()`) from pad @p first, into @p out: one
         *        reading of the matrix's inputs for all.
         * @return How many pads were read (at most `out.size()`; none past the
         *         last pad); 0 before `init()`.
         * @blocking Waits for the registry's lock, once per pad.
         */
        size_t readPins(unsigned first, Span<IoPinRow> out);

        /**
         * @brief Pad @p pad's details (`ioPinDetail()`).
         * @return `Ok`, `Invalid` (not a pad), or `NotReady` before `init()`.
         * @blocking Waits for the registry's lock.
         */
        Error readPin(unsigned pad, IoPinDetail &out);

} // namespace Io

BMDS_NAMESPACE_END
