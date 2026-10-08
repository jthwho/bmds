/**
 * @file      sinetone.h
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
 * @brief A sine at a set frequency and level, written into 32-bit stereo
 *        frames: `audio tone`'s source (`devplan/audio.md` §16).
 *
 * The phase is a 32-bit fraction of a turn, so the frequency is exact to
 * rate / 2³² and the phase never drifts. Each `fill()` fades in over its
 * first few milliseconds after `start()` and out after `fadeOut()`, so the
 * tone starts and stops without a click.
 *
 * @par Thread Safety
 * Not thread-safe; used by one task.
 */
class SineTone {
        public:
                /** @brief Which slots of a frame carry the tone (the others are silent). */
                enum Slots : uint8_t {
                        Left = 1,
                        Right = 2,
                        Both = 3
                };

                /**
                 * @brief Starts a tone of @p frequencyHz at @p rateHz and @p levelCentiDb
                 *        (hundredths of a dB below full scale), in @p slots.
                 */
                void start(uint32_t frequencyHz, uint32_t rateHz, int32_t levelCentiDb, Slots slots);

                /** @brief Begins the fade out; `isFinished()` once it ends. */
                void fadeOut();

                /** @brief Returns true once faded out (or never started). */
                bool isFinished() const { return _gain == 0 && _target == 0; }

                /**
                 * @brief Writes @p frames stereo frames (left, right: 32-bit,
                 *        MSB-justified) into @p out.
                 * @hotpath
                 */
                void fill(int32_t *out, size_t frames);

        private:
                uint32_t _phase = 0;
                uint32_t _increment = 0;
                float    _amplitude = 0;
                float    _gain = 0;     // the fade's position, 0 to 1
                float    _target = 0;   // 0 or 1
                float    _gainStep = 0; // per frame
                Slots    _slots = Both;
};

BMDS_NAMESPACE_END
