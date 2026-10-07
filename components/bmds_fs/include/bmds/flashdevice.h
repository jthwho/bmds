/**
 * @file      flashdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A NOR flash as littlefs uses it: blocks that are erased to 0xFF,
 *        then programmed (devplan/storage.md §13d).
 *
 * The VFS mounts littlefs on a block device whose `BlockDevice::flash()`
 * returns one. A program only clears bits: callers program only erased
 * bytes. Addresses are bytes from the device's start.
 *
 * `FlashPartition` implements it on the target (with an ordered write
 * cache: a write returns before it reaches the flash, and later reads see
 * it), `RamFlashDevice` in memory for the host tests.
 *
 * The owner destroys a device as its own type, never through this interface.
 *
 * @par Thread Safety
 * Thread-safe. `read()`, `program()` and `erase()` may be called from any
 * task, on any stack; each one's effect is seen by every later read.
 */
class FlashDevice {
        public:
                /** @brief Returns the erase block's size in bytes (4096 for NOR flash). */
                virtual uint32_t blockBytes() const = 0;

                /** @brief Returns the number of blocks. */
                virtual uint32_t blockCount() const = 0;

                /**
                 * @brief Reads @p buf's size of bytes at @p address.
                 * @return `Ok`, `OutOfRange`, or a device error.
                 * @blocking Only for a moment (a copy).
                 */
                virtual Error read(uint32_t address, WritableByteSpan buf) = 0;

                /**
                 * @brief Programs @p data at @p address (erased bytes only).
                 * @return `Ok`, `OutOfRange`, or a device error.
                 * @blocking While a write cache has no room.
                 */
                virtual Error program(uint32_t address, ByteSpan data) = 0;

                /**
                 * @brief Erases block @p block to 0xFF.
                 * @return `Ok`, `OutOfRange`, or a device error.
                 * @blocking While a write cache has no room.
                 */
                virtual Error erase(uint32_t block) = 0;

                /**
                 * @brief Waits until everything written is on the flash.
                 * @return `Ok`, or `Timeout`.
                 * @blocking Up to @p timeoutMs (`WaitForever`: until it is).
                 */
                virtual Error flush(unsigned int timeoutMs) = 0;

        protected:
                constexpr FlashDevice() = default;
                ~FlashDevice() = default;
};

BMDS_NAMESPACE_END
