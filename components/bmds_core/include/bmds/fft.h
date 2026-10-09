/**
 * @file      fft.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/memspace.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A fast Fourier transform of real samples, single precision
 *        (`devplan/mediaplayer.md` §6g): radix 2, in place, its tables made
 *        once by `init()`.
 *
 * A transform of N real samples is one of N/2 complex points and a split
 * (the usual packing), so it costs about half a complex one.
 *
 * @par Thread Safety
 * `forward()` only reads the tables: one `Fft` may serve several threads,
 * each with its own data.
 */
class Fft {
        public:
                /** @brief The fewest and most points. */
                static constexpr size_t MinPoints = 4;
                static constexpr size_t MaxPoints = 8192;

                /**
                 * @brief Makes the tables for @p points (a power of two) in @p space.
                 * @return `Ok`, `Invalid` (not a power of two in range) or `NoMem`.
                 */
                Error init(size_t points, MemSpace space);

                /** @brief Returns the points (0 before `init()`). */
                size_t points() const { return _points; }

                /**
                 * @brief Transforms `points()` real samples in @p data, in place, into
                 *        the spectrum, packed: `data[0]` the 0 Hz bin, `data[1]` the
                 *        Nyquist bin (both real), then bin k's real and imaginary parts
                 *        at `data[2k]` and `data[2k + 1]` for k = 1 to N/2 − 1.
                 *        Unscaled (a full-scale sine of N samples: a peak of N/2).
                 * @hotpath
                 */
                void forward(float *data) const;

                /**
                 * @brief Writes the power (|X|²) of the N/2 + 1 bins of @p packed (from
                 *        `forward()`) into @p out.
                 */
                static void power(const float *packed, size_t points, float *out);

        private:
                List<float>    _twiddles{MemSpace::Psram}; // cos, −sin of 2πk/N for k < N/2
                List<uint16_t> _reversed{MemSpace::Psram}; // the bit-reversed index of each of N/2
                size_t         _points = 0;
};

BMDS_NAMESPACE_END
