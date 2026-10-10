/**
 * @file      graphlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A graph's bars, pure (devplan/sysmon.md §3.5).
 */

#include <cstdint>
#include <bmds/graphlayout.h>

BMDS_NAMESPACE_BEGIN

uint16_t GraphLayout::barHeight(int64_t value, int64_t maximum, uint16_t height) {
        if (value <= 0 || maximum <= 0 || height == 0) return 0;
        if (value >= maximum) return height;
        // Rounded to the nearest pixel; anything above zero shows at least one.
        const int64_t h = value <= (INT64_MAX - maximum) / (height + 1) ? (value * height + maximum / 2) / maximum
                                                                        : value / (maximum / height + 1);
        return uint16_t(h > 0 ? h : 1);
}

int64_t GraphLayout::niceMaximum(int64_t value) {
        int64_t decade = 1;
        for (;;) {
                constexpr int64_t Steps[] = {1, 2, 5};
                for (const int64_t step : Steps) {
                        if (step * decade >= value) return step * decade;
                }
                if (decade > INT64_MAX / 100) return INT64_MAX;
                decade *= 10;
        }
}

BMDS_NAMESPACE_END
