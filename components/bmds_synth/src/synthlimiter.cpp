/**
 * @file      synthlimiter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The synths' output limiter (devplan/audio.md §26).
 */

#include <cmath>
#include <bmds/synthlimiter.h>
#include "synthmath.h"

BMDS_NAMESPACE_BEGIN

void SynthLimiter::setup(uint32_t rateHz) {
        _gap = 0.0f;
        // 1 - e^(-1 / (rate x time)): the gap to 1 closes by this much a frame.
        _release = 1.0f - bmdsSynthExpf(-1000.0f / (ReleaseMs * float(rateHz != 0 ? rateHz : 48000)));
        return;
}

uint32_t SynthLimiter::process(float *out, size_t frames, unsigned channels, float peak) {
        // The gap to 1 is kept, not the gain: it shrinks by a factor each frame, which a float does
        // to the end (a gain stepped toward 1 stalls a few ulps short of it, multiplying for good).
        float gap = _gap;
        if (gap == 0.0f) {
                // Not limiting: the block's peak first (fabs and fmax, a sample, unless the caller knew
                // it), and nothing more if it is under the ceiling, as it nearly always is.
                if (peak < 0.0f) {
                        peak = 0.0f;
                        const size_t n = frames * channels;
                        for (size_t i = 0; i < n; ++i) peak = std::fmax(peak, std::fabs(out[i]));
                }
                if (peak <= Ceiling) return 0;
        }
        const float keep = 1.0f - _release;
        uint32_t    limited = 0;
        for (size_t f = 0; f < frames; ++f) {
                float *frame = out + f * channels;
                float  top = 0;
                for (unsigned c = 0; c < channels; ++c) {
                        const float a = frame[c] < 0 ? -frame[c] : frame[c];
                        top = a > top ? a : top;
                }
                if (top * (1.0f - gap) > Ceiling) gap = 1.0f - Ceiling / top; // at once: never past it
                if (gap > 0.0f) {
                        const float g = 1.0f - gap;
                        for (unsigned c = 0; c < channels; ++c) frame[c] *= g;
                        ++limited;
                        gap *= keep;
                        if (gap < 1e-5f) gap = 0.0f; // -0.0001 dB: done
                }
        }
        _gap = gap;
        return limited;
}

BMDS_NAMESPACE_END
