/**
 * @file      intervaltimers.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief A fixed table of interval timers, one per owner (a process's `ITIMER_REAL`:
 *        devplan/programs.md §18), on a clock the caller reads.
 *
 * A timer fires once at its deadline, or repeats: a repeating one moves on by whole
 * intervals past the time it is looked at, so one looked at late fires once, not in a
 * burst. Times are nanoseconds; values past `MaxNs` (about 146 years) are cut to it.
 *
 * @par Thread Safety
 * Not thread-safe: the caller serializes every call (a mutex). No allocation; usable
 * `constinit`.
 *
 * @tparam Capacity How many owners can have a timer at once.
 */
template <size_t Capacity> class IntervalTimers {
        public:
                /** @brief No deadline (`takeDue()` with nothing armed). */
                static constexpr int64_t Never = INT64_MAX;

                /** @brief The longest value or interval kept (longer ones are cut to it). */
                static constexpr uint64_t MaxNs = uint64_t(INT64_MAX / 4);

                /** @brief A timer as set or read. */
                struct Setting {
                                uint64_t valueNs = 0;    ///< Until it fires; 0: disarmed (disarms).
                                uint64_t intervalNs = 0; ///< Then again every so many; 0: once.
                };

                constexpr IntervalTimers() = default;

                IntervalTimers(const IntervalTimers &) = delete;
                IntervalTimers &operator=(const IntervalTimers &) = delete;

                /**
                 * @brief Returns @p owner's timer at @p nowNs: what is left (an armed timer reads
                 *        at least 1, never 0) and its interval; all 0 when disarmed.
                 */
                Setting setting(int32_t owner, int64_t nowNs) const {
                        Setting      s;
                        const Entry *e = find(owner);
                        if (e == nullptr) return s;
                        s.valueNs = e->atNs > nowNs ? static_cast<uint64_t>(e->atNs - nowNs) : 1;
                        s.intervalNs = e->intervalNs;
                        return s;
                }

                /**
                 * @brief Sets @p owner's timer (not 0) from @p nowNs; a `valueNs` of 0 disarms it.
                 * @return `Ok`, `Invalid` (owner 0), or `Full` (no room for another owner).
                 */
                Error set(int32_t owner, int64_t nowNs, Setting s) {
                        if (owner == 0) return Error::Invalid;
                        Entry *e = find(owner);
                        if (s.valueNs == 0) {
                                if (e != nullptr) *e = Entry{};
                                return Error::Ok;
                        }
                        if (e == nullptr) e = find(0);
                        if (e == nullptr) return Error::Full;
                        e->owner = owner;
                        e->atNs = nowNs + static_cast<int64_t>(s.valueNs < MaxNs ? s.valueNs : MaxNs);
                        e->intervalNs = s.intervalNs < MaxNs ? s.intervalNs : MaxNs;
                        return Error::Ok;
                }

                /** @brief Disarms @p owner's timer (it ended). */
                void drop(int32_t owner) {
                        if (Entry *e = owner != 0 ? find(owner) : nullptr; e != nullptr) *e = Entry{};
                        return;
                }

                /**
                 * @brief Takes the timers due at @p nowNs: their owners into @p due (room for
                 *        `Capacity`), one-shot ones disarmed, repeating ones moved on.
                 * @param nextNs Set to the earliest deadline left, or `Never`.
                 * @return How many owners were put in @p due.
                 */
                size_t takeDue(int64_t nowNs, int32_t *due, int64_t &nextNs) {
                        size_t count = 0;
                        nextNs = Never;
                        for (Entry &e : _entries) {
                                if (e.owner == 0) continue;
                                if (e.atNs <= nowNs) {
                                        due[count++] = e.owner;
                                        if (e.intervalNs == 0) {
                                                e = Entry{};
                                                continue;
                                        }
                                        const uint64_t late = static_cast<uint64_t>(nowNs - e.atNs);
                                        e.atNs += static_cast<int64_t>((late / e.intervalNs + 1) * e.intervalNs);
                                }
                                if (e.atNs < nextNs) nextNs = e.atNs;
                        }
                        return count;
                }

                /** @brief Returns how many owners have a timer armed. */
                size_t armed() const {
                        size_t n = 0;
                        for (const Entry &e : _entries) n += e.owner != 0 ? 1 : 0;
                        return n;
                }

        private:
                struct Entry {
                                int32_t  owner = 0; // 0: free
                                int64_t  atNs = 0;
                                uint64_t intervalNs = 0;
                };

                Entry *find(int32_t owner) {
                        for (Entry &e : _entries) {
                                if (e.owner == owner) return &e;
                        }
                        return nullptr;
                }

                const Entry *find(int32_t owner) const {
                        for (const Entry &e : _entries) {
                                if (e.owner == owner) return &e;
                        }
                        return nullptr;
                }

                Entry _entries[Capacity] = {};
};

BMDS_NAMESPACE_END
