/**
 * @file      i2cbus.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

class I2cBus;

/**
 * @brief One address on an I²C bus, at its own clock: what a driver talks to.
 *
 * Made by `I2cBus::device()` and kept for good (a driver asks once, at its
 * start). Transfers on one bus are serialized; each waits for the bus and
 * the transfer together up to its timeout.
 *
 * @par Thread Safety
 * Thread-safe. Not ISR-safe.
 */
class I2cDevice {
        public:
                I2cDevice(const I2cDevice &) = delete;
                I2cDevice &operator=(const I2cDevice &) = delete;

                /** @brief Returns the 7-bit address. */
                uint8_t address() const { return _address; }

                /**
                 * @brief Writes @p data.
                 * @return `Ok`, `Timeout`, or `IoError` (no acknowledge).
                 * @blocking
                 */
                Error write(ByteSpan data, unsigned int timeoutMs);

                /**
                 * @brief Writes @p out, then reads @p in after a repeated start.
                 * @return `Ok`, `Timeout`, or `IoError`.
                 * @blocking
                 */
                Error writeRead(ByteSpan out, WritableByteSpan in, unsigned int timeoutMs);

                /**
                 * @brief Writes @p value to 8-bit register @p reg.
                 * @blocking
                 */
                Error writeRegister(uint8_t reg, uint8_t value, unsigned int timeoutMs);

                /**
                 * @brief Reads 8-bit register @p reg.
                 * @blocking
                 */
                Result<uint8_t> readRegister(uint8_t reg, unsigned int timeoutMs);

        private:
                friend class I2cBus;
                I2cDevice() = default;

                void    *_handle = nullptr; // the IDF device
                uint8_t  _address = 0;
                uint32_t _speedHz = 0;
};

/**
 * @brief An I²C master bus, numbered as the board lists them (`i2c0` is
 *        the board's main bus, `i2c1` the expansion connector's).
 *
 * `init()` starts every bus the board has at boot; drivers then take a
 * `device()` per address they use. Built on IDF's `i2c_master` driver
 * (`devplan/display.md` V3).
 *
 * @par Thread Safety
 * Thread-safe after `init()`.
 */
class I2cBus {
        public:
                /** @brief Devices (address and clock pairs) one bus can hold. */
                static constexpr size_t MaxDevices = 8;

                I2cBus(const I2cBus &) = delete;
                I2cBus &operator=(const I2cBus &) = delete;

                /** @brief Boot stage: starts the board's buses. */
                static Error init();

                /** @brief Returns bus @p number, or null if the board has none such. */
                static I2cBus *bus(size_t number);

                /** @brief Returns the bus number (`i2cN`). */
                size_t number() const { return _number; }

                /** @brief Returns the bus's default clock in Hz. */
                uint32_t clockHz() const { return _clockHz; }

                /**
                 * @brief Returns the device at @p address and @p speedHz (0: the
                 *        bus's clock), made on first use and kept.
                 * @return The device, or `Full` (`MaxDevices`), `Timeout`, or the
                 *         driver's error.
                 * @blocking
                 */
                Result<I2cDevice *> device(uint8_t address, uint32_t speedHz, unsigned int timeoutMs);

                /**
                 * @brief Returns `Ok` if something acknowledges @p address.
                 * @return `Ok`, `NotFound`, or `Timeout`.
                 * @blocking
                 */
                Error probe(uint8_t address, unsigned int timeoutMs);

        private:
                I2cBus(size_t number, void *handle, uint32_t clockHz)
                    : _number(number), _handle(handle), _clockHz(clockHz) {}
                template <typename T> friend class StaticObject;

                size_t    _number;
                void     *_handle; // the IDF bus
                uint32_t  _clockHz;
                Mutex     _lock; // the device table
                I2cDevice _devices[MaxDevices];
                size_t    _deviceCount = 0;
};

BMDS_NAMESPACE_END
