/**
 * @file      clockmodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/** @brief Where the system clock's time came from. Append only. */
enum class ClockSource : uint8_t {
        None,   ///< Never set: counting from `ClockModel::UnsetEpoch`.
        Manual, ///< Set by hand (`date -s`).
        Rtc,    ///< Carried across a reset by the RTC domain.
        Ntp,    ///< Network Time Protocol.
        Ptp,    ///< Precision Time Protocol.
        Gps,    ///< A GNSS receiver.
        Host,   ///< A host computer over a cable.
};

/** @brief Returns a lowercase name for @p source ("manual", "ntp", ...). */
const char *clockSourceName(ClockSource source);

/** @brief The state of the system clock, for display and for discipline loops. */
struct ClockStatus {
                ClockSource source = ClockSource::None; ///< Where the time came from.
                Timestamp   lastSet;                    ///< The time it was last stepped to (zero if never).
                int64_t     slewRemainingNs = 0;        ///< Offset still being slewed in (signed).
                int32_t     frequencyPpb = 0;           ///< Frequency correction in parts per billion.

                /** @brief Returns true if a source has set the clock. */
                bool isSet() const { return source != ClockSource::None; }
};

/**
 * @brief The system clock's arithmetic: wall time as a function of a
 *        monotonic time, as the Linux kernel keeps it.
 *
 * ```
 * wall(m) = anchorWall + (m - anchorMono) * (1 + frequency) + slewed(m)
 * ```
 *
 * - `set()` steps the clock (`settimeofday`), cancelling any slew.
 * - `adjust()` slews an offset in at 500 ppm (`adjtime`), so time never
 *   jumps or runs backwards: 50 ms takes 100 s. A new adjustment replaces
 *   what is left of the previous one.
 * - `setFrequency()` corrects the time base's rate (`adjtimex`), up to
 *   ±500 ppm, for a discipline loop (NTP, PTP).
 *
 * Each change re-takes the anchor at that moment, so the result stays exact
 * over any uptime. Reading is a pure function of the state and `m`; the
 * caller supplies `m` (nanoseconds of a monotonic clock) and the locking.
 *
 * Unset, the clock counts from 2026-01-01 00:00:00 UTC at `m` = 0, so time
 * stamps are plausible and ordered rather than 1970.
 *
 * Fixed size, no padding: it can be held in a `SeqLocked`.
 *
 * @par Thread Safety
 * A plain value: not thread-safe.
 */
class ClockModel {
        public:
                /** @brief Where an unset clock starts: 2026-01-01 00:00:00 UTC. */
                static constexpr Timestamp UnsetEpoch =
                        Timestamp::fromSeconds(Calendar::daysFromCivil(2026, 1, 1) * Calendar::SecondsPerDay);

                /** @brief Largest frequency correction, in ppb (500 ppm). */
                static constexpr int32_t MaxFrequencyPpb = 500'000;

                /** @brief Slew rate: one nanosecond of correction per this many (500 ppm). */
                static constexpr int64_t SlewDivisor = 2000;

                /** @brief Constructs an unset clock. */
                constexpr ClockModel() = default;

                /** @brief Returns the wall time at monotonic time @p monoNs. */
                Timestamp wallAt(int64_t monoNs) const;

                /** @brief Steps the clock to @p wall at @p monoNs; cancels any slew. */
                void set(int64_t monoNs, Timestamp wall, ClockSource source);

                /**
                 * @brief Starts slewing @p offsetNs in from @p monoNs, replacing what was left.
                 * @return The part of the previous adjustment that hadn't been applied.
                 */
                int64_t adjust(int64_t monoNs, int64_t offsetNs);

                /**
                 * @brief Sets the frequency correction from @p monoNs on.
                 * @return `Ok`, or `OutOfRange` beyond ±`MaxFrequencyPpb`.
                 */
                Error setFrequency(int64_t monoNs, int32_t ppb);

                /** @brief Returns the source, last set time, remaining slew and frequency. */
                ClockStatus status(int64_t monoNs) const;

                /** @brief Returns where the time came from. */
                ClockSource source() const { return static_cast<ClockSource>(_source); }

                /** @brief Returns the frequency correction in ppb. */
                int32_t frequencyPpb() const { return _frequencyPpb; }

        private:
                int64_t slewed(int64_t monoNs) const;
                void    reanchor(int64_t monoNs);

                int64_t _anchorMono = 0;
                int64_t _anchorWall = UnsetEpoch.ns;
                int64_t _slewTotal = 0; // The adjustment being slewed in (signed).
                int64_t _slewStart = 0; // Monotonic time it started.
                int64_t _lastSet = 0;
                int32_t _frequencyPpb = 0;
                uint8_t _source = 0;
                uint8_t _reserved[3] = {};
};

BMDS_NAMESPACE_END
