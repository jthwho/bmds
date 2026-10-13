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

        inline constexpr Board BoardDefinition = {
                .name = "Olimex ESP32-P4-PC",
                .xtalHz = 40'000'000,
                .flashBytes = 16u * 1024u * 1024u, // W25Q128JV
                .psramBytes = 32u * 1024u * 1024u, // In-package (ESP32-P4NRW32)
                .memSpaces =
                        {
                                // Indexed by MemSpace.
                                {.heapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
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
                .usbSerialJtag = true, // USB-C (GPIO24/25)
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
                // The ESP32-P4's JPEG codec (TRM: JPEG Codec): baseline decoding and encoding.
                .jpegCodecs = 1,
        };

} // namespace Detail

BMDS_NAMESPACE_END
