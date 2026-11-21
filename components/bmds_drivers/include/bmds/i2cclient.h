/**
 * @file      i2cclient.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/i2cbus.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An open `/dev/i2c-N` (devplan/io.md §11.3): its own target address and force flag, as
 *        Linux's i2c-dev's `i2c_client` per open file. `read()` and `write()` are a plain read
 *        or write at the target; `i2c_ctl` sets the target and makes raw transfers.
 *
 * Made by the device file's open, in PSRAM; it frees itself when its handle goes.
 *
 * @par Thread Safety
 * Any task; the bus keeps transactions whole.
 */
class I2cClient final : public Stream {
        public:
                /** @brief No target yet. */
                static constexpr int NoTarget = -1;

                /** @brief The most bytes one read or write moves (Linux's i2c-dev: 8192). */
                static constexpr size_t MaxBytes = 8192;

                /** @brief A transfer's wait for the bus and the device, ms (Linux's default: 1 s). */
                static constexpr unsigned int TimeoutMs = 1000;

                explicit I2cClient(I2cBus &bus) : _bus(bus) {}

                /** @brief Its bus. */
                I2cBus &bus() const { return _bus; }

                /** @brief Its target address, or `NoTarget`. */
                int target() const { return _target; }

                /** @brief True if it may reach addresses a BMDS driver owns. */
                bool isForced() const { return _force; }

                /**
                 * @brief Sets the target (Linux's `I2C_SLAVE`, `_FORCE`): @p force lets it reach an
                 *        address a BMDS driver owns, and any after it.
                 * @return `Ok`, `Invalid` (past 0x7f), `Busy` (a driver's, without force).
                 */
                Error setTarget(uint8_t address, bool force);

                /** @brief True if this file may reach @p address (not a driver's, or forced). */
                bool mayReach(uint8_t address) const { return _force || !_bus.isOwned(address); }

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                I2cClient     *i2cClient() override { return this; }
                void           handleClosed(uint8_t mode) override;

        private:
                I2cBus &_bus;
                int     _target = NoTarget;
                bool    _force = false;
};

BMDS_NAMESPACE_END
