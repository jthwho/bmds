/**
 * @file      resampler.h
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
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Converts stereo float from one rate to another by bandlimited
 *        interpolation (J. O. Smith's), for one stream (`devplan/audio.md`
 *        §17).
 *
 * One Kaiser-windowed sinc table, made at compile time, serves every pair
 * of rates: points between its entries are interpolated linearly. A ratio
 * with few enough phases (out / gcd(in, out)) gets each phase's
 * coefficients worked out once, at `configure()`. Going
 * down, the kernel is stretched so its cutoff follows the output's Nyquist
 * frequency. The position is kept exactly (a whole input frame, and a
 * fraction in units of 1/out), so nothing drifts.
 *
 * Use, each period: `inputNeeded()` frames into `inputSpace()`, `commit()`,
 * then `process()`, which adds its output into a mix. The first output is
 * `delayFrames()` input frames late: the filter's half length.
 *
 * @par Thread Safety
 * Not thread-safe: one stream's, used by the engine's task.
 */
class Resampler {
        public:
                /** @brief Zero crossings of the sinc on each side (taps per side going up). */
                static constexpr unsigned ZeroCrossings = 16;

                /** @brief Table entries per zero crossing. */
                static constexpr unsigned TableDensity = 256;

                /**
                 * @brief The most floats of precomputed rows a stream takes (64 KB):
                 *        44.1, 22.05, 32, 16, 8 and 96 kHz to 48 kHz fit; 11.025 kHz
                 *        (640 phases) interpolates.
                 */
                static constexpr size_t MaxRowFloats = 16384;

                Resampler() = default;
                ~Resampler();
                Resampler(const Resampler &) = delete;
                Resampler &operator=(const Resampler &) = delete;

                /**
                 * @brief Sets the rates and allocates the history for outputs of up to
                 *        @p maxOutFrames at a time, in @p space. Forgets the history.
                 * @return `Ok`, `Invalid` (a rate of 0), or `NoMem`.
                 */
                Error configure(uint32_t inRate, uint32_t outRate, size_t maxOutFrames, MemSpace space);

                /** @brief Frees the history. */
                void release();

                /** @brief Forgets the history (silence before the next input). */
                void reset();

                /** @brief Returns the input rate. */
                uint32_t inRate() const { return _inRate; }

                /** @brief Returns the output rate. */
                uint32_t outRate() const { return _outRate; }

                /** @brief Returns the input frames between an input and its output. */
                size_t delayFrames() const { return _halfTaps; }

                /**
                 * @brief Returns the input frames to add so that `process()` can make
                 *        @p outFrames (at most the configured most).
                 * @nonblocking
                 */
                size_t inputNeeded(size_t outFrames) const;

                /** @brief Returns where @p frames input frames go (null: more than fit). */
                float *inputSpace(size_t frames);

                /** @brief Takes @p frames frames written into `inputSpace()`. */
                void commit(size_t frames);

                /**
                 * @brief Adds up to @p outFrames output frames, times @p gain, into
                 *        @p mix (interleaved stereo), and drops the input no longer
                 *        needed.
                 * @return The frames made: fewer if the input ran short.
                 * @hotpath
                 */
                size_t process(float *mix, size_t outFrames, float gain);

        private:
                float   *_buffer = nullptr; // stereo frames: history, then input
                size_t   _capacity = 0;     // frames
                size_t   _count = 0;        // frames in _buffer
                size_t   _position = 0;     // the next output's left input frame
                uint32_t _fraction = 0;     // and how far past it, in 1/_outRate
                uint32_t _inRate = 0;
                uint32_t _outRate = 0;
                uint32_t _stepWhole = 0; // input frames an output advances
                uint32_t _stepFraction = 0;
                size_t   _halfTaps = 0;   // input frames each side
                float    _scale = 1.0f;   // out/in going down, else 1
                float    _tableStep = 0;  // table entries per input frame
                float   *_rows = nullptr; // each phase's coefficients, left then right, if they fit
                size_t   _side = 0;       // coefficients a side in a row
                uint32_t _phaseUnit = 1;  // gcd(in, out): _fraction / it is the phase
};

BMDS_NAMESPACE_END
