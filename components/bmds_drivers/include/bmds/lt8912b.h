/**
 * @file      lt8912b.h
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
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The Lontium LT8912B MIPI-DSI to HDMI bridge, on I²C
 *        (`devplan/display.md`).
 *
 * The chip answers at three consecutive addresses, its register pages:
 * main (the board's address, 0x48), CEC/DSI (+1) and AVI infoframe (+2).
 * It has no hot-plug or interrupt line on this board: the monitor's
 * presence is read from a register.
 *
 * @par Thread Safety
 * Not thread-safe; owned by the display.
 */
class Lt8912b {
        public:
                /** @brief The chip id in main registers 0x00 and 0x01. */
                static constexpr uint16_t ChipId = 0x12b2;

                /** @brief Constructs a bridge whose main page is @p mainAddress on @p bus. */
                Lt8912b(I2cBus &bus, uint8_t mainAddress) : _bus(bus), _mainAddress(mainAddress) {}

                Lt8912b(const Lt8912b &) = delete;
                Lt8912b &operator=(const Lt8912b &) = delete;

                /**
                 * @brief Takes the bus devices for the three pages.
                 * @return `Ok`, or the bus's error.
                 * @blocking
                 */
                Error attach();

                /**
                 * @brief Reads the chip id (`ChipId` for an LT8912B).
                 * @return The id, or the bus's error (`IoError`: no answer).
                 * @blocking
                 */
                Result<uint16_t> chipId();

                /**
                 * @brief Returns true while a monitor is connected (its hot-plug line).
                 * @blocking
                 */
                Result<bool> isMonitorConnected();

                /** @brief Returns the bus. */
                I2cBus &bus() { return _bus; }

                /** @brief Returns the main page's address. */
                uint8_t address() const { return _mainAddress; }

        private:
                static constexpr unsigned int TimeoutMs = 50;

                I2cBus    &_bus;
                uint8_t    _mainAddress;
                I2cDevice *_main = nullptr;
                I2cDevice *_dsi = nullptr;
                I2cDevice *_avi = nullptr;
};

BMDS_NAMESPACE_END
