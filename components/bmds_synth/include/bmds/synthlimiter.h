/**
 * @file      synthlimiter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The synths' output limiter (`devplan/audio.md` §26): a frame that would pass the ceiling
 *        (-1 dBFS) is turned down to it at once, and the gain comes back over `ReleaseMs`. Below
 *        the ceiling with the gain back at 1, samples pass untouched. So a synth can play louder
 *        (`synth/gain`) without its rare peaks clipping.
 *
 * Per frame: the larger channel's magnitude, a compare, the gain's step back, a multiply a
 * sample; a division only on a frame over the ceiling.
 *
 * @par Thread Safety
 * Not thread-safe: its synth's.
 */
class SynthLimiter {
        public:
                static constexpr float Ceiling = 0.891251f; ///< -1 dBFS.
                static constexpr float ReleaseMs = 50.0f;   ///< The gain's way back (time constant).

                /** @brief Sets it up for @p rateHz, the gain at 1. */
                void setup(uint32_t rateHz);

                /**
                 * @brief Limits @p frames frames of @p out (interleaved, @p channels) in place;
                 *        @p peak their largest magnitude when the caller knows it (else below 0: it is
                 *        found here).
                 * @return The frames it turned down (over the ceiling, or still coming back).
                 * @hotpath
                 */
                uint32_t process(float *out, size_t frames, unsigned channels, float peak = -1.0f);

                /** @brief Returns the gain now (1: not limiting). */
                float gain() const { return 1.0f - _gap; }

        private:
                float _gap = 0.0f;     // 1 - the gain
                float _release = 0.0f; // the gap's share closed a frame
};

BMDS_NAMESPACE_END
