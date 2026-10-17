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
#include <bmds/edid.h>
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

                /**
                 * @brief Sets the bridge up for @p timing from @p lanes DSI data
                 *        lanes (1-4), sending HDMI (with an AVI infoframe) or DVI,
                 *        and turns its HDMI output on. Call before the DSI video
                 *        starts; `resetReceiver()` after.
                 * @return `Ok`, or the bus's error.
                 * @blocking
                 */
                Error configure(const VideoTiming &timing, uint8_t lanes, bool hdmi);

                /**
                 * @brief Resets the DSI receiver and the pixel clock recovery, so
                 *        they lock onto the video now coming in.
                 * @blocking
                 */
                Error resetReceiver();

                /**
                 * @brief Turns the HDMI output on or off (the TMDS drivers).
                 * @blocking
                 */
                Error setOutput(bool on);

                /**
                 * @brief Sets the DSI receiver's settle count for the next `configure()` (0: the
                 *        usual 0x10, right for the 720p link). A test aid (`display try`).
                 */
                void setSettle(uint8_t settle) {
                        _settle = settle;
                        return;
                }

                /** @brief Sets the DDS frequency word for the next `configure()` (0: the table's). A test aid. */
                void setDds(uint32_t word) {
                        _dds = word;
                        return;
                }

                /** @brief Returns the settle count the last `configure()` used. */
                uint8_t settle() const { return _settleUsed; }

                /**
                 * @brief Reads what the bridge measures of its DSI input: main 0x9c-0x9d and
                 *        0x9e-0x9f (lines a frame: 750 for the 720p link), low byte first.
                 * @blocking
                 */
                Error measured(uint16_t &first, uint16_t &lines);

                /**
                 * @brief Reads register @p reg of page @p page (0 main, 1 DSI, 2 AVI). A test aid
                 *        (`display regs`).
                 * @return The value, `Invalid` (the page), `InvalidState` (not attached), or the
                 *         bus's error.
                 * @blocking
                 */
                Result<uint8_t> readRegister(uint8_t page, uint8_t reg);

                /**
                 * @brief Writes @p value to register @p reg of page @p page. A test aid
                 *        (`display chip set`): the next `configure()` writes its own again.
                 * @return `Ok`, `Invalid`, `InvalidState`, or the bus's error.
                 * @blocking
                 */
                Error writeRegister(uint8_t page, uint8_t reg, uint8_t value);

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
                uint8_t    _settle = 0;     // setSettle()'s
                uint8_t    _settleUsed = 0; // the last configure()'s
                uint32_t   _dds = 0;        // setDds()'s
};

BMDS_NAMESPACE_END
