/**
 * @file      board.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Board description types and access to the selected board definition.
 *
 * All hardware truth (pins, peripheral instances, bus speeds, memory
 * mapping, which devices exist) lives in the board definition, a single
 * `constexpr Board` in `boards/<board>/bmds/boarddef.h`, selected by
 * Kconfig (`BMDS_BOARD`). Drivers take their configuration from
 * `bmds::board()`; they never hard-code a GPIO number.
 *
 * A field set to `NoGpio` (or a count of 0) means the board doesn't have
 * that signal or device.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/** @brief A GPIO number. */
using GpioNum = int8_t;

/** @brief "No GPIO": the signal is not connected to the SoC. */
inline constexpr GpioNum NoGpio = -1;

/** @brief "No LDO": the supply is not from an on-chip LDO channel. */
inline constexpr int8_t NoLdo = -1;

/** @brief A GPIO used as a digital signal with a defined active level. */
struct GpioLine {
                GpioNum gpio = NoGpio;     ///< GPIO number, or `NoGpio`.
                bool    activeLow = false; ///< True if the signal is asserted by driving/reading low.

                /** @brief Returns true if the signal is connected. */
                constexpr bool isValid() const { return gpio != NoGpio; }
};

/** @brief How a MemSpace maps onto the ESP-IDF heap. */
struct BoardMemSpace {
                uint32_t heapCaps = 0; ///< `MALLOC_CAP_*` flags used to allocate from this space.
};

/** @brief An I2C bus. */
struct BoardI2cBus {
                uint8_t  port = 0;     ///< I2C controller index.
                GpioNum  sda = NoGpio; ///< SDA GPIO.
                GpioNum  scl = NoGpio; ///< SCL GPIO.
                uint32_t clockHz = 0;  ///< Bus clock.

                /** @brief Returns true if the bus exists. */
                constexpr bool isValid() const { return sda != NoGpio && scl != NoGpio; }
};

/** @brief Index of a board I2C bus in `Board::i2c`. */
enum class BoardI2c : uint8_t {
        Main, ///< On-board devices (codec, display bridge, HDMI DDC, camera/panel connectors).
        Ext,  ///< Expansion connector (UEXT).
};

/** @brief Number of `BoardI2c` buses. */
inline constexpr size_t BoardI2cCount = 2;

/** @brief Serial console UART (in addition to the USB-Serial/JTAG console). */
struct BoardUart {
                uint8_t port = 0;    ///< UART controller index.
                GpioNum tx = NoGpio; ///< TX GPIO.
                GpioNum rx = NoGpio; ///< RX GPIO.
};

/** @brief microSD card on the SDMMC host. */
struct BoardSdCard {
                uint8_t  slot = 0;        ///< SDMMC host slot.
                uint8_t  busWidth = 0;    ///< Data lines (1 or 4); 0 = no SD card.
                GpioNum  clk = NoGpio;    ///< CLK GPIO.
                GpioNum  cmd = NoGpio;    ///< CMD GPIO.
                GpioNum  d0 = NoGpio;     ///< D0 GPIO.
                GpioNum  d1 = NoGpio;     ///< D1 GPIO.
                GpioNum  d2 = NoGpio;     ///< D2 GPIO.
                GpioNum  d3 = NoGpio;     ///< D3 GPIO.
                GpioLine cardDetect = {}; ///< Card-detect switch (asserted = card present).
                GpioLine powerOn = {};    ///< Card VDD switch (asserted = card powered).
                int8_t   ioLdo = NoLdo;   ///< On-chip LDO channel supplying the SD IO domain.
                uint16_t ioLdoMv = 0;     ///< IO LDO voltage in mV.
};

/** @brief MIPI-DSI to HDMI bridge chips. */
enum class BoardDisplayBridge : uint8_t {
        None,    ///< No display.
        Lt8912b, ///< Lontium LT8912B MIPI-DSI to HDMI.
};

/** @brief Display output. */
struct BoardDisplay {
                BoardDisplayBridge bridge = BoardDisplayBridge::None; ///< Bridge chip.
                BoardI2c           i2c = BoardI2c::Main;              ///< Bus the bridge is controlled on.
                uint8_t            i2cAddr = 0;                       ///< Bridge 7-bit I2C address (main page).
                uint8_t            dsiLanes = 0;                      ///< MIPI-DSI data lanes.
                uint16_t           laneMbps = 0;                      ///< DSI lane bit rate.
                int8_t             phyLdo = NoLdo;                    ///< On-chip LDO channel for the DSI PHY.
                uint16_t           phyLdoMv = 0;                      ///< DSI PHY LDO voltage in mV.
                GpioLine           reset = {};                        ///< Bridge reset (asserted = in reset).
                bool               hasDdc = false;                    ///< HDMI DDC (EDID) reachable on `i2c`.
};

/** @brief USB host port (USB 2.0 HS OTG, possibly behind a hub). */
struct BoardUsbHost {
                uint8_t  portCount = 0;  ///< User-facing ports; 0 = no USB host.
                bool     hasHub = false; ///< Ports are behind an on-board hub.
                GpioLine hubReset = {};  ///< Hub reset (asserted = in reset).
                GpioLine vbusOn = {};    ///< VBUS enable, if SoC-controlled (asserted = powered).
};

/** @brief Ethernet PHY chips. */
enum class BoardEthPhy : uint8_t {
        None,    ///< No Ethernet.
        Ip101gr, ///< IC+ IP101GR(R) 10/100.
};

/** @brief RMII Ethernet. */
struct BoardEthernet {
                BoardEthPhy phy = BoardEthPhy::None; ///< PHY chip.
                uint8_t     phyAddr = 0;             ///< PHY MDIO address.
                GpioLine    phyReset = {};           ///< PHY reset (asserted = in reset).
                GpioNum     mdc = NoGpio;            ///< MDC GPIO.
                GpioNum     mdio = NoGpio;           ///< MDIO GPIO.
                GpioNum     refClk = NoGpio;         ///< 50 MHz RMII reference clock GPIO.
                bool        refClkIn = true;         ///< True if REF_CLK is supplied by the PHY (input to SoC).
                GpioNum     txEn = NoGpio;           ///< TX_EN GPIO.
                GpioNum     txd0 = NoGpio;           ///< TXD0 GPIO.
                GpioNum     txd1 = NoGpio;           ///< TXD1 GPIO.
                GpioNum     crsDv = NoGpio;          ///< CRS_DV GPIO.
                GpioNum     rxd0 = NoGpio;           ///< RXD0 GPIO.
                GpioNum     rxd1 = NoGpio;           ///< RXD1 GPIO.
};

/** @brief Audio codec chips. */
enum class BoardAudioCodec : uint8_t {
        None,   ///< No audio.
        Es8311, ///< Everest ES8311 mono codec.
};

/** @brief I2S audio codec. */
struct BoardAudio {
                BoardAudioCodec codec = BoardAudioCodec::None; ///< Codec chip.
                BoardI2c        i2c = BoardI2c::Main;          ///< Bus the codec is controlled on.
                uint8_t         i2cAddr = 0;                   ///< Codec 7-bit I2C address.
                GpioNum         mclk = NoGpio;                 ///< I2S MCLK GPIO.
                GpioNum         bclk = NoGpio;                 ///< I2S BCLK GPIO.
                GpioNum         ws = NoGpio;                   ///< I2S WS / LRCK GPIO.
                GpioNum         dout = NoGpio;                 ///< I2S data out of the SoC (to the DAC).
                GpioNum         din = NoGpio;                  ///< I2S data into the SoC (from the ADC).
                GpioLine        powerOn = {};                  ///< Codec supply enable (asserted = powered).
};

/** @brief A complete board description. */
struct Board {
                const char   *name = "";                     ///< Human-readable board name.
                uint32_t      xtalHz = 0;                    ///< Main crystal frequency.
                uint32_t      flashBytes = 0;                ///< Flash size.
                uint32_t      psramBytes = 0;                ///< PSRAM size (0 = none).
                BoardMemSpace memSpaces[MemSpaceCount] = {}; ///< Indexed by `MemSpace`.
                BoardI2cBus   i2c[BoardI2cCount] = {};       ///< Indexed by `BoardI2c`.
                BoardUart     uart = {};                     ///< Auxiliary serial UART.
                GpioLine      statusLed = {};                ///< User LED (asserted = lit).
                GpioLine      userButton = {};               ///< User button (asserted = pressed).
                BoardSdCard   sdCard = {};                   ///< microSD.
                BoardDisplay  display = {};                  ///< Display output.
                BoardUsbHost  usbHost = {};                  ///< USB host.
                BoardEthernet ethernet = {};                 ///< Ethernet.
                BoardAudio    audio = {};                    ///< Audio.

                /** @brief Returns the heap mapping for @p space. */
                constexpr const BoardMemSpace &memSpace(MemSpace space) const {
                        return memSpaces[static_cast<size_t>(space)];
                }

                /** @brief Returns the description of I2C bus @p bus. */
                constexpr const BoardI2cBus &i2cBus(BoardI2c bus) const { return i2c[static_cast<size_t>(bus)]; }
};

BMDS_NAMESPACE_END

// The selected board's definition (found via the board include directory).
#include <bmds/boarddef.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Returns the selected board's definition.
 * @nonblocking @isrsafe
 */
constexpr const Board &board() {
        return Detail::BoardDefinition;
}

BMDS_NAMESPACE_END
