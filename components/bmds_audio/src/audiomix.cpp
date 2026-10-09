/**
 * @file      audiomix.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The limiter, the mix to the DMA's slots and gains (devplan/audio.md §17).
 */

#include <cmath>
#include <bmds/audiomix.h>

BMDS_NAMESPACE_BEGIN

void AudioLimiter::setRate(uint32_t rateHz) {
        _cut = 0.0f;
        _limited = 0;
        _release =
                rateHz == 0 ? 0.0f : std::exp(-1000.0f / (static_cast<float>(ReleaseMs) * static_cast<float>(rateHz)));
        return;
}

void AudioLimiter::process(float *stereo, size_t frames) {
        // The gain's distance below 1: kept as that, since 1 - d rounds to a float near 1
        // in steps too coarse for the recovery's (it would stall at about 1 - 7e-5).
        float d = _cut;
        for (size_t i = 0; i < frames; ++i) {
                float      &l = stereo[2 * i];
                float      &r = stereo[2 * i + 1];
                const float peak = std::fmax(std::fabs(l), std::fabs(r));
                if (d > 0.0f) {
                        d *= _release;
                        if (d < 1e-6f) d = 0.0f; // no denormals on the way up
                }
                if (peak * (1.0f - d) > 1.0f) {
                        d = 1.0f - 1.0f / peak;
                        ++_limited;
                }
                if (d > 0.0f) {
                        const float g = 1.0f - d;
                        l *= g;
                        r *= g;
                }
        }
        _cut = d;
        return;
}

namespace AudioMix {

        namespace {

                // Full scale in the slots: the largest float below 2^31 (2^31 itself overflows).
                constexpr float SlotMax = 2147483520.0f;

                inline int32_t slot(float x) {
                        const float v = x * 2147483648.0f;
                        if (v >= SlotMax) return INT32_MAX;
                        if (!(v > -2147483648.0f)) return INT32_MIN; // NaN too
                        return static_cast<int32_t>(v);
                }

        } // namespace

        void toSlots(const float *stereo, int32_t *slots, size_t frames, bool mono) {
                if (mono) {
                        for (size_t i = 0; i < frames; ++i) {
                                const int32_t v = slot(0.5f * (stereo[2 * i] + stereo[2 * i + 1]));
                                slots[2 * i] = v;
                                slots[2 * i + 1] = v;
                        }
                        return;
                }
                for (size_t i = 0; i < 2 * frames; ++i) slots[i] = slot(stereo[i]);
                return;
        }

        float gainOf(int32_t centiDb) {
                return std::pow(10.0f, static_cast<float>(centiDb) / 2000.0f);
        }

} // namespace AudioMix

BMDS_NAMESPACE_END
