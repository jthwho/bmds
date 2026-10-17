/**
 * @file      videomodes.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/edid.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Video mode timings: the standard ones (CEA-861, VESA DMT) by size
 *        and rate, and VESA CVT reduced blanking for any other.
 *
 * @par Thread Safety
 * Pure functions.
 */
namespace VideoModes {

        /**
         * @brief The timing for @p width × @p height at @p hz: a standard mode
         *        when there is one (1280×720 at 60 Hz is CEA's, 74.25 MHz),
         *        else CVT reduced blanking (v1: 160 pixels of horizontal
         *        blanking, at least 460 µs of vertical).
         * @return The timing, or `Invalid` (a zero, or a rate above 240 Hz).
         */
        Result<VideoTiming> timing(uint16_t width, uint16_t height, uint16_t hz);

        /**
         * @brief Parses "WIDTHxHEIGHT@HZ" (e.g. "1280x720@60") into its timing.
         * @return The timing, or `Invalid`.
         */
        Result<VideoTiming> parse(StringView text);

        /**
         * @brief Fits @p timing to the nearest pixel clock at least as fast
         *        that @p sourceKhz (each a PLL, e.g. 240 and 160 MHz) divided by
         *        an integer makes, keeping its rate: the horizontal front porch
         *        grows to take up the faster clock (the vertical total too when
         *        a line would get too long for the hardware: 4095 pixels).
         * @param minKhz The slowest clock to use (a bridge's floor), or 0.
         * @param maxKhz The fastest clock the link takes.
         * @return The fitted timing, or `NotSupported` (no clock fits).
         */
        Result<VideoTiming> fitToClock(const VideoTiming &timing, const uint32_t *sourceKhz, size_t sources,
                                       uint32_t minKhz, uint32_t maxKhz);

        /**
         * @brief The mode for a full-screen picture of @p width × @p height at the shape
         *        @p aspectX : @p aspectY (both 0: square pixels) that @p monitor takes and the
         *        picture fills exactly by whole factors both ways: VGA's 640×400 at 70 Hz (mode
         *        13h's), 640×480 at 60 Hz or 720×400 at 70 Hz, each shown at 4:3
         *        (`devplan/games.md` §20).
         * @return The timing, or `NotFound` (none: scale on the desktop's mode), `Invalid`.
         */
        Result<VideoTiming> fullScreen(uint16_t width, uint16_t height, uint32_t aspectX, uint32_t aspectY,
                                       const Edid &monitor);

        /**
         * @brief The least margin, in percent, of a DSI link's lanes over its pixels: sent
         *        in burst mode with none, the HDMI bridge lost the picture now and then
         *        (`devplan/display.md` §5e).
         */
        inline constexpr uint32_t LaneMarginPercent = 15;

        /**
         * @brief The lane rate in Mbps for sending @p timing as 24-bit pixels over @p lanes:
         *        the slowest at least `LaneMarginPercent` over the pixels' rate that a PLL of
         *        reference @p refMhz makes exactly (the DSI PHY's: M/N times the reference,
         *        M even, the reference over N 5 to 40 MHz) and that makes a line a whole
         *        number of lane byte clocks. Up to @p maxMbps; with no such rate, the margin
         *        gives way first (the slowest rate both ways above the pixels'), then the line
         *        (the slowest the PLL makes), then the PLL (the pixels' rate itself).
         * @return The rate, or 0 (the pixels' rate is above @p maxMbps, or a zero argument).
         */
        uint32_t laneMbps(const VideoTiming &timing, uint8_t lanes, uint32_t refMhz, uint32_t maxMbps);

        /** @brief CVT reduced blanking (v1) for @p width × @p height at @p hz. */
        VideoTiming cvtReducedBlanking(uint16_t width, uint16_t height, uint16_t hz);

} // namespace VideoModes

BMDS_NAMESPACE_END
