/**
 * @file      edid.h
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
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A video mode's timing: what a display controller and an HDMI
 *        bridge are programmed with.
 *
 * Horizontal values are pixels, vertical ones lines; a line (or frame) is
 * active + front porch + sync + back porch.
 */
struct VideoTiming {
                uint32_t pixelKhz = 0;       ///< Pixel clock in kHz.
                uint16_t hActive = 0;        ///< Visible pixels per line.
                uint16_t hFront = 0;         ///< Front porch.
                uint16_t hSync = 0;          ///< Sync pulse.
                uint16_t hBack = 0;          ///< Back porch.
                uint16_t vActive = 0;        ///< Visible lines.
                uint16_t vFront = 0;         ///< Front porch.
                uint16_t vSync = 0;          ///< Sync pulse.
                uint16_t vBack = 0;          ///< Back porch.
                bool     hSyncHigh = false;  ///< Horizontal sync is active high.
                bool     vSyncHigh = false;  ///< Vertical sync is active high.
                bool     interlaced = false; ///< Interlaced (fields, not frames).

                /** @brief Pixels per line, blanking included. */
                constexpr uint32_t hTotal() const { return uint32_t(hActive) + hFront + hSync + hBack; }

                /** @brief Lines per frame, blanking included. */
                constexpr uint32_t vTotal() const { return uint32_t(vActive) + vFront + vSync + vBack; }

                /** @brief Frames per second in thousandths (0 if unset). */
                constexpr uint32_t refreshMilliHz() const {
                        const uint64_t pixels = uint64_t(hTotal()) * vTotal();
                        return pixels == 0 ? 0 : static_cast<uint32_t>(uint64_t(pixelKhz) * 1000000 / pixels);
                }
};

/**
 * @brief A monitor's EDID (VESA E-EDID 1.3/1.4): who it is and what mode it
 *        prefers, from its 128-byte base block and an optional CEA-861
 *        extension block.
 *
 * Only what BMDS uses is kept: the maker and product, the name, the
 * preferred mode (the first detailed timing) and whether it is an HDMI
 * sink (a CEA extension with the HDMI vendor block) rather than DVI.
 *
 * @par Thread Safety
 * A value type; `parse()` and `parseExtension()` are pure.
 */
struct Edid {
                /** @brief Bytes in an EDID block. */
                static constexpr size_t BlockBytes = 128;

                char        maker[4] = {};   ///< Three-letter PNP id, e.g. "DEL".
                uint16_t    product = 0;     ///< Maker's product code.
                uint8_t     version = 0;     ///< EDID version (1).
                uint8_t     revision = 0;    ///< EDID revision (3 or 4).
                bool        digital = false; ///< Digital input.
                char        name[14] = {};   ///< Monitor name descriptor, trimmed; "" if none.
                VideoTiming preferred;       ///< The preferred mode (`pixelKhz` 0 if none).
                uint8_t     extensions = 0;  ///< Extension blocks that follow the base block.
                bool        hdmi = false;    ///< An HDMI sink (from the CEA extension).

                /** @brief A mode named by size and rate (standard, established, CEA). */
                struct Mode {
                                uint16_t width = 0;
                                uint16_t height = 0;
                                uint16_t hz = 0;
                                bool     interlaced = false;
                };

                /** @brief The monitor's range limits descriptor (`present` false if none). */
                struct Range {
                                bool     present = false;
                                uint16_t minHz = 0; ///< Vertical rate, Hz.
                                uint16_t maxHz = 0;
                                uint16_t minKhz = 0; ///< Horizontal (line) rate, kHz.
                                uint16_t maxKhz = 0;
                                uint32_t maxPixelKhz = 0; ///< Highest pixel clock (0 if unstated).
                };

                static constexpr size_t MaxDetailed = 4; ///< In the base block.
                static constexpr size_t MaxStandard = 8;
                static constexpr size_t MaxVideoModes = 32; ///< CEA short video descriptors kept.
                static constexpr size_t MaxExtDetailed = 6; ///< Detailed timings kept from the extension.

                uint8_t     week = 0;    ///< Week of manufacture (0 unknown, 255: `year` is a model year).
                uint16_t    year = 0;    ///< Year of manufacture.
                uint8_t     widthCm = 0; ///< Screen size (0 if unstated).
                uint8_t     heightCm = 0;
                char        serial[14] = {}; ///< Serial number descriptor; "" if none.
                Range       range;
                uint32_t    established = 0; ///< Established timing bits: bytes 35-37 as bits 23-0.
                Mode        standard[MaxStandard];
                uint8_t     standardCount = 0;
                VideoTiming detailed[MaxDetailed]; ///< The base block's detailed timings (the first is preferred).
                uint8_t     detailedCount = 0;
                uint8_t     videoModes[MaxVideoModes] = {}; ///< CEA short video descriptors, as sent.
                uint8_t     videoModeCount = 0;
                VideoTiming extDetailed[MaxExtDetailed]; ///< The CEA extension's detailed timings.
                uint8_t     extDetailedCount = 0;

                /**
                 * @brief Parses a base block.
                 * @return The EDID, or `CorruptData` (bad header or checksum),
                 *         `Invalid` (@p block isn't 128 bytes).
                 */
                static Result<Edid> parse(ByteSpan block);

                /**
                 * @brief Takes what BMDS uses from an extension block (a CEA-861
                 *        one: whether the sink is HDMI, its video modes and
                 *        detailed timings); other kinds are skipped.
                 * @return `Ok`, or `CorruptData` (bad checksum), `Invalid` (size).
                 */
                Error parseExtension(ByteSpan block);

                /**
                 * @brief Decodes an 18-byte detailed timing descriptor.
                 * @return False if it isn't one (a display descriptor).
                 */
                static bool parseDetailedTiming(const uint8_t *d, VideoTiming &out);

                /**
                 * @brief The mode of established timing bit @p bit (23 is byte 35's
                 *        bit 7, 7 is byte 37's).
                 * @return False for a bit with no mode.
                 */
                static bool establishedMode(int bit, Mode &out);

                /**
                 * @brief The CEA-861 mode of short video descriptor @p svd; its
                 *        number in @p vic, whether the sink calls it native.
                 * @return False if the number isn't one BMDS knows by size.
                 */
                static bool videoMode(uint8_t svd, Mode &out, uint8_t &vic, bool &native);
};

BMDS_NAMESPACE_END
