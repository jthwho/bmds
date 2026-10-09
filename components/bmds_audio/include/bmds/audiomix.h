/**
 * @file      audiomix.h
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
 * @brief Keeps a mix within full scale without touching what is already
 *        within it (`devplan/audio.md` §17).
 *
 * A frame whose louder channel would pass full scale sets the gain to bring
 * it to full scale at once, so nothing overshoots; the gain then recovers
 * toward 1 with a time constant of `ReleaseMs`. A mix that stays within
 * full scale passes unchanged.
 *
 * @par Thread Safety
 * Not thread-safe: the engine's.
 */
class AudioLimiter {
        public:
                /** @brief The recovery's time constant. */
                static constexpr uint32_t ReleaseMs = 50;

                /** @brief Sets the rate (the recovery's step) and forgets the gain. */
                void setRate(uint32_t rateHz);

                /**
                 * @brief Limits @p frames stereo frames at @p stereo in place.
                 * @hotpath
                 */
                void process(float *stereo, size_t frames);

                /** @brief Returns the frames it has reduced since `setRate()`. */
                uint32_t limitedFrames() const { return _limited; }

                /** @brief Returns the gain now (1: not limiting). */
                float gain() const { return 1.0f - _cut; }

        private:
                float    _cut = 0.0f;     // the gain's distance below 1
                float    _release = 0.0f; // per frame, of the distance from 1
                uint32_t _limited = 0;
};

/**
 * @brief The engine's edges: the mix to the DMA's slots, and gains
 *        (`devplan/audio.md` §17).
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace AudioMix {

        /**
         * @brief Writes @p frames stereo float frames as 32-bit stereo slots
         *        (MSB-justified, clamped). A @p mono device gets (L + R) / 2 in
         *        both slots.
         * @hotpath
         */
        void toSlots(const float *stereo, int32_t *slots, size_t frames, bool mono);

        /** @brief The quietest volume in centi-dB (the floor a mute goes to). */
        inline constexpr int32_t MutedCentiDb = -9600;


        /** @brief Returns @p centiDb as an amplitude (0 dB: 1). */
        float gainOf(int32_t centiDb);

} // namespace AudioMix

BMDS_NAMESPACE_END
