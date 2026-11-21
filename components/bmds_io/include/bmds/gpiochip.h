/**
 * @file      gpiochip.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The chip's pads as a device file, `/dev/gpiochip0` (devplan/io.md §3.9): what
 *        `gpio_ctl` operates on (the loader's), and Linux's GPIO character device through
 *        `linux.c`. Its reads come from `Io` (<bmds/io.h>).
 *
 * One for the kernel's life; every fd on it shares it. Reads and writes are refused: it is only
 * operated on.
 *
 * @par Thread Safety
 * Any task.
 */
class GpioChip final : public Stream {
        public:
                /** @brief Adds `/dev/gpiochip0`. After `Io::init()` and the VFS; once. */
                static Error addFiles();

                Result<size_t> read(WritableByteSpan, unsigned int) override {
                        return makeError<size_t>(Error::Invalid);
                }
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                GpioChip      *gpioChip() override { return this; }

                /** @brief Returns N of `/dev/gpiochipN`. */
                uint32_t index() const { return _index; }

        private:
                explicit GpioChip(uint32_t index) : _index(index) {}

                uint32_t _index;
};

BMDS_NAMESPACE_END
