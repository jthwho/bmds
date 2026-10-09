/**
 * @file      spectrum.h
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
#include <bmds/fft.h>
#include <bmds/list.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The visualizer's analysis (`devplan/mediaplayer.md` §6g), pure:
 *        samples through a Hann window and an FFT into `Bands` bands from
 *        `LowHz` to `HighHz` on a log scale, in dB (a full-scale sine 0 dB).
 *
 * A band narrower than a bin takes the bin it falls in, so the low bands of a
 * short transform repeat.
 *
 * @par Thread Safety
 * `analyse()` only reads its tables; the work buffer is the caller's.
 */
class Spectrum {
        public:
                static constexpr size_t Bands = 32;
                static constexpr float  LowHz = 40;
                static constexpr float  HighHz = 16000;
                static constexpr float  FloorDb = -90; ///< Silence.

                /**
                 * @brief Prepares for @p points samples (a power of two) at @p rateHz.
                 * @return `Ok`, `Invalid` or `NoMem`.
                 */
                Error init(size_t points, uint32_t rateHz, MemSpace space);

                /** @brief Returns the points it takes. */
                size_t points() const { return _fft.points(); }

                /**
                 * @brief Analyses `points()` samples (full scale ±1) into @p db
                 *        (`Bands` values), using @p work (`points()` floats).
                 * @hotpath
                 */
                void analyse(const float *samples, float *work, float *db) const;

        private:
                Fft            _fft;
                List<float>    _window{MemSpace::Psram};
                List<uint16_t> _edges{MemSpace::Psram}; // band i: bins [edges[2i], edges[2i + 1])
                float          _reference = 1;          // a full-scale sine's band power
};

/**
 * @brief Meter bars' movement (`devplan/mediaplayer.md` §6g), pure: each bar
 *        jumps up to a new level and falls `FallDbPerSecond`; a peak mark
 *        holds `HoldMs`, then falls too.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class MeterBars {
        public:
                static constexpr size_t   MaxBars = 32;
                static constexpr float    FallDbPerSecond = 20;
                static constexpr uint32_t HoldMs = 1000;

                /** @brief Bars at @p floorDb (`MaxBars` at most). */
                MeterBars(size_t bars, float floorDb);

                /** @brief Takes new levels (@p db, a value a bar) @p elapsedMs after the last. */
                void update(const float *db, uint32_t elapsedMs);

                /** @brief Falls with no new levels (paused, silence), for @p elapsedMs. */
                void fall(uint32_t elapsedMs);

                /** @brief Returns bar @p i's level. */
                float level(size_t i) const { return _level[i]; }

                /** @brief Returns bar @p i's peak mark. */
                float peak(size_t i) const { return _peak[i]; }

                /** @brief Returns true when every bar and mark is at the floor (nothing left to draw). */
                bool isAtFloor() const;

                /** @brief Puts every bar and mark at the floor. */
                void reset();

        private:
                size_t   _bars;
                float    _floor;
                float    _level[MaxBars];
                float    _peak[MaxBars];
                uint32_t _held[MaxBars]; // ms the mark has held
};

BMDS_NAMESPACE_END
