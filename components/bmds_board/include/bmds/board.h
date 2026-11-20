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

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/memspace.h>
#include <bmds/span.h>

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
                GpioNum     ppsOut = NoGpio;         ///< The 1588 clock's PPS output (1 Hz, rising on the second).
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

/**
 * @brief Reads the board's serial number into @p out (@p size bytes, NUL ended):
 *        each board says where its own is (`devplan/notifications.md` §2).
 * @return Its length (0: none).
 */
using BoardSerialReader = size_t (*)(char *out, size_t size);

/**
 * @brief The chip's factory MAC address from its eFuses, as 12 lower-case hex
 *        digits: a board's serial reader when the chip's identity is the board's.
 */
size_t chipMacSerial(char *out, size_t size);

/** @brief A board without a serial number: "" (the default reader). */
inline size_t noSerial(char *out, size_t size) {
        if (out != nullptr && size > 0) out[0] = '\0';
        return 0;
}

/**
 * @brief What a pad is to the board (`devplan/io.md` §3.1-§3.3).
 */
enum class BoardPinUse : uint8_t {
        Fixed,  ///< Wired to something a change would break (a crystal, the USB console): changed only by force.
        Device, ///< Wired to an on-board device, whose driver claims it while it runs.
        Free,   ///< Brought out to a connector or unused on the board: the user's.
};

/** @brief A pad the board connects (`Board::pins`). */
struct BoardPin {
                GpioNum     gpio = NoGpio;           ///< The GPIO.
                BoardPinUse use = BoardPinUse::Free; ///< What it is to the board.
                const char *label = "";              ///< What it is wired to (`UEXT UART0 TX`), or "".
                const char *note = "";               ///< On-board parts on it, jumpers, caveats; or "".
};

/** @brief A pin of a connector (`BoardConnector::pins`), in the connector's numbering from 1. */
struct BoardConnectorPin {
                GpioNum     gpio = NoGpio; ///< The GPIO on it, or `NoGpio` for a supply or another signal.
                const char *label = "";    ///< For a pin without a GPIO: `3.3V`, `GND`, `EN`.
};

/** @brief A connector the board brings pads out to (`Board::connectors`). */
struct BoardConnector {
                const char                   *name = "";        ///< Its silkscreen name (`UEXT`).
                const char                   *description = ""; ///< What it is, for people.
                uint8_t                       columns = 1;      ///< Rows of pins: pin 1 first, across a row.
                bool                          fitted = true;    ///< False for a footprint without the header.
                Span<const BoardConnectorPin> pins;             ///< Pin 1 first.
};

/** @brief A complete board description. */
struct Board {
                const char   *name = "";                     ///< Human-readable board name.
                uint32_t      xtalHz = 0;                    ///< Main crystal frequency.
                uint32_t      flashBytes = 0;                ///< Flash size.
                uint32_t      psramBytes = 0;                ///< PSRAM size (0 = none).
                BoardMemSpace memSpaces[MemSpaceCount] = {}; ///< Indexed by `MemSpace`.
                BoardI2cBus   i2c[BoardI2cCount] = {};       ///< Indexed by `BoardI2c`.
                bool          usbSerialJtag = false; ///< USB-Serial/JTAG is wired to a connector (the console port).
                bool              usbDeviceOnConsole = false; ///< The OTG FS controller can take the console port.
                BoardUart     uart = {};             ///< Auxiliary serial UART.
                GpioLine      statusLed = {};        ///< User LED (asserted = lit).
                GpioLine      userButton = {};       ///< User button (asserted = pressed).
                BoardSdCard   sdCard = {};           ///< microSD.
                BoardDisplay  display = {};          ///< Display output.
                BoardUsbHost  usbHost = {};          ///< USB host.
                BoardEthernet ethernet = {};         ///< Ethernet.
                BoardAudio    audio = {};            ///< Audio.
                BoardSerialReader serial = noSerial;     ///< Reads its serial number.
                Span<const BoardPin>       pins; ///< Every pad the board connects, by GPIO (`devplan/io.md` §3.1).
                Span<const BoardConnector> connectors;            ///< Where pads are brought out.
                uint8_t           jpegCodecs = 0;        ///< JPEG codec blocks (`devplan/images.md` §3c): `jpeg0`...

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

namespace Detail {

        // The board's pin table against the rest of its definition (devplan/io.md §3.1): checked
        // at compile time, so the table the I/O view shows can't drift from what drivers use.

        constexpr bool boardHasPin(const Board &b, GpioNum gpio) {
                if (gpio == NoGpio) return true;
                for (const BoardPin &p : b.pins)
                        if (p.gpio == gpio) return true;
                return false;
        }

        constexpr bool boardPinsSorted(const Board &b) {
                for (size_t i = 0; i < b.pins.size(); ++i) {
                        if (b.pins[i].gpio == NoGpio) return false;
                        if (i > 0 && b.pins[i].gpio <= b.pins[i - 1].gpio) return false;
                }
                return true;
        }

        constexpr bool boardDevicePinsListed(const Board &b) {
                const GpioNum used[] = {
                        b.i2c[0].sda,
                        b.i2c[0].scl,
                        b.i2c[1].sda,
                        b.i2c[1].scl,
                        b.uart.tx,
                        b.uart.rx,
                        b.statusLed.gpio,
                        b.userButton.gpio,
                        b.sdCard.clk,
                        b.sdCard.cmd,
                        b.sdCard.d0,
                        b.sdCard.d1,
                        b.sdCard.d2,
                        b.sdCard.d3,
                        b.sdCard.cardDetect.gpio,
                        b.sdCard.powerOn.gpio,
                        b.display.reset.gpio,
                        b.usbHost.hubReset.gpio,
                        b.usbHost.vbusOn.gpio,
                        b.ethernet.phyReset.gpio,
                        b.ethernet.mdc,
                        b.ethernet.mdio,
                        b.ethernet.refClk,
                        b.ethernet.txEn,
                        b.ethernet.txd0,
                        b.ethernet.txd1,
                        b.ethernet.crsDv,
                        b.ethernet.rxd0,
                        b.ethernet.rxd1,
                        b.ethernet.ppsOut,
                        b.audio.mclk,
                        b.audio.bclk,
                        b.audio.ws,
                        b.audio.dout,
                        b.audio.din,
                        b.audio.powerOn.gpio,
                };
                for (GpioNum g : used)
                        if (!boardHasPin(b, g)) return false;
                return true;
        }

        constexpr bool boardConnectorPinsListed(const Board &b) {
                for (const BoardConnector &c : b.connectors)
                        for (const BoardConnectorPin &p : c.pins)
                                if (!boardHasPin(b, p.gpio)) return false;
                return true;
        }

} // namespace Detail

static_assert(Detail::boardPinsSorted(board()), "the board's pins must be listed once each, by GPIO number");
static_assert(Detail::boardDevicePinsListed(board()), "a GPIO the board's devices use is missing from its pins");
static_assert(Detail::boardConnectorPinsListed(board()), "a connector's GPIO is missing from the board's pins");

BMDS_NAMESPACE_END
