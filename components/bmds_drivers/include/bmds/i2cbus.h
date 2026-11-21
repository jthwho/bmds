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
#include <utility>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ioregistry.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

class I2cBus;

/**
 * @brief One message of a raw transfer (`I2cBus::transfer()`): Linux's `i2c_msg` in shape.
 *
 * @par Thread Safety
 * A plain value.
 */
struct I2cMessage {
                uint16_t address = 0;    ///< The 7-bit address.
                bool     read = false;   ///< Read (else write).
                uint8_t *data = nullptr; ///< The bytes written, or where the bytes read go.
                size_t   length = 0;     ///< How many (0: the address alone, a probe).
};

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

                /** @brief Commands one transfer may take (the controller's command registers). */
                static constexpr size_t MaxCommands = 8;

                /**
                 * @brief Sends @p messages as one transaction, a repeated start between each and
                 *        one stop at the end (devplan/io.md §11.3): a write is a start, its address
                 *        and its bytes (3 commands), a read a start, its address and its bytes (up to
                 *        4); at most `MaxCommands` in all with the stop: a write then a read fits.
                 *        Between the drivers' own transactions (the bus's lock).
                 * @return `Ok`; `NotFound` (an address or a byte not acknowledged), `Timeout`,
                 *         `Invalid` (an address past 0x7f, too many commands), `NoMem`.
                 * @blocking
                 */
                Error transfer(Span<const I2cMessage> messages, unsigned int timeoutMs);

                /** @brief True if a BMDS driver uses @p address on this bus (`device()`'s). */
                bool isOwned(uint8_t address) const;

                /** @brief The buses there are (their numbers: 0 up). */
                static size_t count();

                /**
                 * @brief Adds `/dev/i2c-N` for each bus (devplan/io.md §11.3): a file per open, its
                 *        own target address (`I2cClient`). After the buses and the VFS; once.
                 * @return `Ok`, or `Vfs::addDeviceFile()`'s error.
                 */
                static Error addFiles();

        private:
                I2cBus(size_t number, void *handle, uint32_t clockHz, IoClaim &&claim)
                    : _number(number), _handle(handle), _clockHz(clockHz), _claim(std::move(claim)) {}
                template <typename T> friend class StaticObject;

                size_t        _number;
                void         *_handle; // the IDF bus
                uint32_t      _clockHz;
                IoClaim       _claim;         // its pads and controller (devplan/io.md §3.2)
                mutable Mutex _lock;          // the device table, _raw
                void         *_raw = nullptr; // the device raw transfers go through, made when first wanted
                I2cDevice     _devices[MaxDevices];
                size_t        _deviceCount = 0;
};

BMDS_NAMESPACE_END
