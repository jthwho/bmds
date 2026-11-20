/**
 * @file      boarddef.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Board definition: Olimex ESP32-P4-PC (Rev B / Rev C).
 *
 * Source: the Rev C KiCad schematic (netlist export), cross-checked against
 * Rev B (identical SoC pin assignments) and Olimex's production-test
 * firmware. See docs/HARDWARE.md for details and references. Do not include
 * directly; use <bmds/board.h>.
 */

#pragma once

#include <esp_heap_caps.h>

// Included from the end of <bmds/board.h>, after the Board types are defined.

BMDS_NAMESPACE_BEGIN

namespace Detail {

        // Every pad the board connects (devplan/io.md §3.1), from the Rev C schematic and PCB
        // netlist; docs/HARDWARE.md's pin map says the same for people. Fixed pads are changed only
        // by force; Device pads are claimed by their drivers while they run; Free pads are the
        // user's (on a connector, or a board part nothing in BMDS drives: the LED, the sense lines).
        inline constexpr BoardPin BoardPins[] = {
                {0, BoardPinUse::Fixed, "32.768 kHz crystal", "Q1"},
                {1, BoardPinUse::Fixed, "32.768 kHz crystal", "Q1"},
                {2, BoardPinUse::Free, "User LED (green)", "Active low"},
                {3, BoardPinUse::Device, "SD card detect", "Active low; its pull-up is powered by LDO VO4"},
                {4, BoardPinUse::Free, "UEXT SPI SCK", ""},
                {5, BoardPinUse::Free, "UEXT SPI CS", ""},
                {6, BoardPinUse::Device, "Codec power", "Pulled up (on); low switches the codec's LDO off"},
                {7, BoardPinUse::Device, "Main I2C SDA", "10k pull-up; display bridge, codec, HDMI DDC"},
                {8, BoardPinUse::Device, "Main I2C SCL", "10k pull-up"},
                {9, BoardPinUse::Device, "I2S DOUT (codec DSDIN)", "33R series"},
                {10, BoardPinUse::Device, "I2S WS (codec LRCK)", ""},
                {11, BoardPinUse::Device, "I2S DIN (codec ASDOUT)", ""},
                {12, BoardPinUse::Device, "I2S BCLK (codec SCLK)", ""},
                {13, BoardPinUse::Device, "I2S MCLK", ""},
                {14, BoardPinUse::Free, "", ""},
                {15, BoardPinUse::Free, "", ""},
                {16, BoardPinUse::Free, "", ""},
                {17, BoardPinUse::Free, "", ""},
                {18, BoardPinUse::Free, "", ""},
                {19, BoardPinUse::Free, "", ""},
                {20, BoardPinUse::Free, "Battery sense (VBAT/2)", "Needs the BAT_SENS_E1/E2 jumpers closed"},
                {21, BoardPinUse::Device, "USB hub reset", "Active low; 10k pull-up"},
                {22, BoardPinUse::Device, "UEXT I2C SDA", "2.2k pull-up"},
                {23, BoardPinUse::Device, "UEXT I2C SCL", "2.2k pull-up"},
                {24, BoardPinUse::Fixed, "USB-C D- (USB-Serial/JTAG)", "The console"},
                {25, BoardPinUse::Fixed, "USB-C D+ (USB-Serial/JTAG)", "The console"},
                {26, BoardPinUse::Device, "PPS out (the 1588 clock)", "The second FS PHY's D-"},
                {27, BoardPinUse::Free, "", "The second FS PHY's D+"},
                {28, BoardPinUse::Device, "RMII CRS_DV", ""},
                {29, BoardPinUse::Device, "RMII RXD0", ""},
                {30, BoardPinUse::Device, "RMII RXD1", ""},
                {31, BoardPinUse::Device, "Ethernet MDC", "1.5k pull-up"},
                {32, BoardPinUse::Free, "External 5 V sense", "Digital only; needs the PWR_SENS_E1 jumper closed"},
                {33, BoardPinUse::Free, "", ""},
                {34, BoardPinUse::Device, "RMII TXD0", ""},
                {35, BoardPinUse::Device, "RMII TXD1 / BOOT button", "10k pull-up; the button pulls it low"},
                {36, BoardPinUse::Free, "", "10k pull-up"},
                {37, BoardPinUse::Free, "UEXT UART0 TX", ""},
                {38, BoardPinUse::Free, "UEXT UART0 RX", "4.7k pull-up"},
                {39, BoardPinUse::Device, "SD D0", "4.7k pull-up to LDO VO4"},
                {40, BoardPinUse::Device, "SD D1", "4.7k pull-up to LDO VO4"},
                {41, BoardPinUse::Device, "SD D2", "4.7k pull-up to LDO VO4"},
                {42, BoardPinUse::Device, "SD D3", "4.7k pull-up to LDO VO4"},
                {43, BoardPinUse::Device, "SD CLK", ""},
                {44, BoardPinUse::Device, "SD CMD", "4.7k pull-up to LDO VO4"},
                {45, BoardPinUse::Device, "SD card power", "Active low (P-FET); on by default"},
                {46, BoardPinUse::Free, "", ""},
                {47, BoardPinUse::Free, "", ""},
                {48, BoardPinUse::Free, "", ""},
                {49, BoardPinUse::Device, "RMII TX_EN", ""},
                {50, BoardPinUse::Device, "RMII REF_CLK (50 MHz in)", "From the PHY"},
                {51, BoardPinUse::Device, "Ethernet PHY reset", "Active low; nothing else drives it"},
                {52, BoardPinUse::Device, "Ethernet MDIO", "1.5k pull-up"},
                {53, BoardPinUse::Free, "UEXT SPI MOSI", ""},
                {54, BoardPinUse::Free, "UEXT SPI MISO", ""},
        };

        // UEXT (UEXT1, Olimex's 10-pin expansion connector) and EXT1 (2x10, not fitted), from the
        // Rev C PCB netlist (2026-11-20).
        inline constexpr BoardConnectorPin UextPins[] = {
                {NoGpio, "3.3V"}, {NoGpio, "GND"}, {37, ""}, {38, ""}, {23, ""},
                {22, ""},         {54, ""},        {53, ""}, {4, ""},  {5, ""},
        };

        inline constexpr BoardConnectorPin Ext1Pins[] = {
                {NoGpio, "3.3V"}, {NoGpio, "5V"}, {NoGpio, "GND"}, {26, ""}, {NoGpio, "EN"}, {27, ""}, {2, ""},
                {14, ""},         {15, ""},       {16, ""},        {17, ""}, {18, ""},       {19, ""}, {20, ""},
                {32, ""},         {33, ""},       {36, ""},        {46, ""}, {47, ""},       {48, ""},
        };

        inline constexpr BoardConnector BoardConnectors[] = {
                {"UEXT", "UEXT: UART0, I2C1, SPI", 2, true, UextPins},
                {"EXT1", "EXT1: 20-pin GPIO header", 2, false, Ext1Pins},
        };

        inline constexpr Board BoardDefinition = {
                .name = "Olimex ESP32-P4-PC",
                .xtalHz = 40'000'000,
                .flashBytes = 16u * 1024u * 1024u, // W25Q128JV
                .psramBytes = 32u * 1024u * 1024u, // In-package (ESP32-P4NRW32)
                .memSpaces =
                        {
                                // Indexed by MemSpace. Internal is the L2 RAM, all of it DMA-capable:
                                // asking for DMA keeps out the LP RAM (slow, and its 128-bit loads
                                // come back wrong: PIE kernels got garbage there once the L2 RAM was
                                // short) and the TCM, which the heap otherwise gives an internal
                                // request when nothing else is left. (Not MALLOC_CAP_SIMD: ESP-IDF
                                // registers its PSRAM reserve of L2 RAM without it.)
                                {.heapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT},
                                {.heapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT},
                                {.heapCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
                                {.heapCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED},
                                {.heapCaps = MALLOC_CAP_TCM},
                        },
                .i2c =
                        {
                                // Main: LT8912B (0x48-0x4A), ES8311 (0x18), HDMI DDC (0x50), DSI/CSI connectors.
                                // 10k pull-ups on board. 100 kHz: at 400 kHz about 40% of reads
                                // from the LT8912B came back wrong, and writes were corrupted.
                                {.port = 0, .sda = 7, .scl = 8, .clockHz = 100'000},
                                // UEXT pins 5/6, 2.2k pull-ups.
                                {.port = 1, .sda = 22, .scl = 23, .clockHz = 100'000},
                        },
                // UART0 on UEXT pins 3/4 (the primary console is USB-Serial/JTAG on USB-C).
                .usbSerialJtag = true,      // USB-C (GPIO24/25)
                .usbDeviceOnConsole = true, // the full-speed OTG controller on the USB-C's PHY
                .uart = {.port = 0, .tx = 37, .rx = 38},
                .statusLed = {.gpio = 2, .activeLow = true},
                // BOOT button; shared with RMII TXD1 and a strapping pin, so only usable when Ethernet is down.
                .userButton = {.gpio = 35, .activeLow = true},
                .sdCard =
                        {
                                .slot = 0, // Dedicated IOMUX pins
                                .busWidth = 4,
                                .clk = 43,
                                .cmd = 44,
                                .d0 = 39,
                                .d1 = 40,
                                .d2 = 41,
                                .d3 = 42,
                                // Pull-up is powered from LDO VO4: only valid while ioLdo is on.
                                // Low with a card in (confirmed on a Rev C board).
                                .cardDetect = {.gpio = 3, .activeLow = true},
                                // P-FET high-side switch, gate pulled low: powered by default.
                                .powerOn = {.gpio = 45, .activeLow = true},
                                .ioLdo = 4,
                                .ioLdoMv = 3300,
                        },
                .display =
                        {
                                .bridge = BoardDisplayBridge::Lt8912b,
                                .i2c = BoardI2c::Main,
                                .i2cAddr = 0x48,
                                .dsiLanes = 2,
                                .laneMbps = 1000,
                                .phyLdo = 3,
                                .phyLdoMv = 2500,
                                .reset = {}, // RC reset only; no interrupt or HPD GPIO either
                                .hasDdc = true,
                        },
                .usbHost =
                        {
                                .portCount = 4,
                                .hasHub = true, // Terminus FE1.1s
                                .hubReset = {.gpio = 21, .activeLow = true},
                                .vbusOn = {}, // CH217K switch controlled by the hub
                        },
                .ethernet =
                        {
                                .phy = BoardEthPhy::Ip101gr,
                                .phyAddr = 1,
                                .phyReset = {.gpio = 51, .activeLow = true},
                                .mdc = 31,
                                .mdio = 52,
                                .refClk = 50,
                                .refClkIn = true,
                                .txEn = 49,
                                .txd0 = 34,
                                .txd1 = 35,
                                .crsDv = 28,
                                .rxd0 = 29,
                                .rxd1 = 30,
                                // EXT1 pin 4 (the header isn't fitted): the second FS PHY's D-,
                                // which nothing here uses (USB is on the UTMI PHY).
                                .ppsOut = 26,
                        },
                .audio =
                        {
                                .codec = BoardAudioCodec::Es8311,
                                .i2c = BoardI2c::Main,
                                .i2cAddr = 0x18,
                                .mclk = 13,
                                .bclk = 12,
                                .ws = 10,
                                .dout = 9,
                                .din = 11,
                                // Drives the codec LDO's CE; pulled up, so powered by default.
                                .powerOn = {.gpio = 6, .activeLow = false},
                        },
                // No serial number of its own: the chip's factory MAC address.
                .serial = chipMacSerial,
                .pins = BoardPins,
                .connectors = BoardConnectors,
                // The ESP32-P4's JPEG codec (TRM: JPEG Codec): baseline decoding and encoding.
                .jpegCodecs = 1,
        };

} // namespace Detail

BMDS_NAMESPACE_END
