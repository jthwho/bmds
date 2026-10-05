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

                /**
                 * @brief Parses a base block.
                 * @return The EDID, or `CorruptData` (bad header or checksum),
                 *         `Invalid` (@p block isn't 128 bytes).
                 */
                static Result<Edid> parse(ByteSpan block);

                /**
                 * @brief Takes what BMDS uses from an extension block (a CEA-861
                 *        one: whether the sink is HDMI); other kinds are skipped.
                 * @return `Ok`, or `CorruptData` (bad checksum), `Invalid` (size).
                 */
                Error parseExtension(ByteSpan block);

                /**
                 * @brief Decodes an 18-byte detailed timing descriptor.
                 * @return False if it isn't one (a display descriptor).
                 */
                static bool parseDetailedTiming(const uint8_t *d, VideoTiming &out);
};

BMDS_NAMESPACE_END
