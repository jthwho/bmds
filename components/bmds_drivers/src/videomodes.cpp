/**
 * @file      videomodes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Timings from CEA-861 (the HDMI video formats) and VESA DMT, and the VESA
 * CVT reduced-blanking formula (v1) for everything else.
 */

#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct Standard {
                        uint16_t    hz;
                        VideoTiming timing;
        };

        // pixel kHz, h active/front/sync/back, v active/front/sync/back, h+, v+.
        constexpr Standard Standards[] = {
                {60, {25'175, 640, 16, 96, 48, 480, 10, 2, 33, false, false, false}},   // DMT
                {60, {40'000, 800, 40, 128, 88, 600, 1, 4, 23, true, true, false}},     // DMT
                {60, {65'000, 1024, 24, 136, 160, 768, 3, 6, 29, false, false, false}}, // DMT
                {60, {74'250, 1280, 110, 40, 220, 720, 5, 5, 20, true, true, false}},   // CEA 4
                {50, {74'250, 1280, 440, 40, 220, 720, 5, 5, 20, true, true, false}},   // CEA 19
                {60, {108'000, 1280, 48, 112, 248, 1024, 1, 3, 38, true, true, false}}, // DMT
                {30, {74'250, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false}},   // CEA 34
                {25, {74'250, 1920, 528, 44, 148, 1080, 4, 5, 36, true, true, false}},  // CEA 33
                {24, {74'250, 1920, 638, 44, 148, 1080, 4, 5, 36, true, true, false}},  // CEA 32
        };

        constexpr uint32_t MinVBlankUs = 460;
        constexpr uint16_t VFront = 3;
        constexpr uint16_t MinVBack = 6;

        // CVT's vertical sync by aspect ratio.
        uint16_t cvtVSync(uint16_t w, uint16_t h) {
                if (w * 3 == h * 4) return 4;
                if (w * 9 == h * 16) return 5;
                if (w * 10 == h * 16) return 6;
                if (w * 4 == h * 5 || w * 3 == h * 5) return 7; // 5:4, 15:9
                return 10;
        }

        bool parseNumber(StringView &text, char end, uint32_t &out) {
                uint32_t v = 0;
                size_t   i = 0;
                for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                        v = v * 10 + static_cast<uint32_t>(text[i] - '0');
                        if (v > 65535) return false;
                }
                if (i == 0) return false;
                if (end != '\0') {
                        if (i == text.size() || text[i] != end) return false;
                        ++i;
                } else if (i != text.size()) {
                        return false;
                }
                text = text.mid(i);
                out = v;
                return true;
        }

} // namespace

VideoTiming VideoModes::cvtReducedBlanking(uint16_t width, uint16_t height, uint16_t hz) {
        VideoTiming t;
        t.hActive = width;
        t.hFront = 48;
        t.hSync = 32;
        t.hBack = 80;
        t.vActive = height;
        t.vFront = VFront;
        t.vSync = cvtVSync(width, height);
        // The line period from the frame period less the minimum blanking, then
        // enough lines of blanking to cover that minimum.
        const uint64_t frameNs = 1'000'000'000ull / hz;
        const uint64_t lineNs = (frameNs - MinVBlankUs * 1000ull) / height;
        const uint32_t blankLines = static_cast<uint32_t>(MinVBlankUs * 1000ull / lineNs) + 1; // CVT: INT(...) + 1
        const uint32_t back = blankLines > uint32_t(VFront) + t.vSync ? blankLines - VFront - t.vSync : 0;
        t.vBack = static_cast<uint16_t>(back < MinVBack ? MinVBack : back);
        t.pixelKhz = static_cast<uint32_t>(uint64_t(t.hTotal()) * t.vTotal() * hz / 1000);
        t.hSyncHigh = true;
        t.vSyncHigh = false;
        return t;
}

Result<VideoTiming> VideoModes::timing(uint16_t width, uint16_t height, uint16_t hz) {
        if (width == 0 || height == 0 || hz == 0 || hz > 240) return makeError<VideoTiming>(Error::Invalid);
        for (const Standard &s : Standards) {
                if (s.timing.hActive == width && s.timing.vActive == height && s.hz == hz) return makeResult(s.timing);
        }
        return makeResult(cvtReducedBlanking(width, height, hz));
}

Result<VideoTiming> VideoModes::fitToClock(const VideoTiming &timing, const uint32_t *sourceKhz, size_t sources,
                                           uint32_t maxKhz) {
        constexpr uint32_t MaxHTotal = 4095;
        const uint32_t     milliHz = timing.refreshMilliHz();
        if (milliHz == 0) return makeError<VideoTiming>(Error::Invalid);
        // The slowest divided clock at least as fast as the timing's own.
        uint32_t best = 0;
        for (size_t i = 0; i < sources; ++i) {
                for (uint32_t div = 1; div <= 256; ++div) {
                        if (sourceKhz[i] % div != 0) continue;
                        const uint32_t khz = sourceKhz[i] / div;
                        if (khz < timing.pixelKhz) break;
                        if (khz <= maxKhz && (best == 0 || khz < best)) best = khz;
                }
        }
        if (best == 0) return makeError<VideoTiming>(Error::NotSupported);
        VideoTiming t = timing;
        t.pixelKhz = best;
        // Pixels a frame at the faster clock, shared out as lines of the same
        // count: a longer horizontal blank (more lines if a line would be too long).
        const uint64_t framePixels = uint64_t(best) * 1000 * 1000 / milliHz;
        uint32_t       vTotal = timing.vTotal();
        while (framePixels / vTotal > MaxHTotal) ++vTotal;
        const uint32_t hTotal = static_cast<uint32_t>((framePixels + vTotal / 2) / vTotal);
        t.vBack = static_cast<uint16_t>(t.vBack + (vTotal - timing.vTotal()));
        t.hFront = static_cast<uint16_t>(t.hFront + (hTotal - timing.hTotal()));
        return makeResult(t);
}

Result<VideoTiming> VideoModes::parse(StringView text) {
        uint32_t w = 0, h = 0, hz = 0;
        if (!parseNumber(text, 'x', w) || !parseNumber(text, '@', h) || !parseNumber(text, '\0', hz))
                return makeError<VideoTiming>(Error::Invalid);
        return timing(static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(hz));
}

BMDS_NAMESPACE_END
