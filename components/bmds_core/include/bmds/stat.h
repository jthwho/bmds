/**
 * @file      stat.h
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
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system monitor's counters, pure (`devplan/sysmon.md` §3.2, §3.3):
 *        what a counter is, the rules that judge it, the log of problems, the
 *        overrides from `sysmon/limits`, and the history kept while watched.
 *        The registry that holds the counters is the kernel's (`Stats`).
 *
 * Names, units, kinds and state labels are an interface once shipped: append
 * only.
 */
namespace Stat {

        /** @brief A value that can't be read now (CPU time not counted this boot, a clock not yet measured). */
        constexpr int64_t Unavailable = INT64_MIN;

        /** @brief A threshold not used (`Limits::warn` or `crit`). */
        constexpr int64_t NoLevel = INT64_MIN;

        /** @brief A counter's longest name, and a group's longest prefix. */
        constexpr size_t MaxName = 47;

        /** @brief What a counter's value is measured in. */
        enum class Unit : uint8_t {
                Count = 0,
                Bytes = 1,
                Micros = 2,   ///< µs.
                Permille = 3, ///< ‰.
                Hz = 4,
                MilliHz = 5,
                Ppm = 6,
                Frames = 7,
                Cycles = 8,
                Decibels = 9,
                Nanos = 10,
                Ppb = 11,
        };

        /** @brief How a counter's value moves. */
        enum class Kind : uint8_t {
                Total = 0, ///< Running, only grows (a restart may reset it): shown as a total and a rate.
                Level = 1, ///< A value now.
                Peak = 2,  ///< A running largest (a restart may reset it).
                State = 3, ///< A number with labels (`Desc::labels`).
                Info = 4,  ///< A fact that seldom changes (a rate, a size).
        };

        /** @brief How a counter is judged. */
        enum class Rule : uint8_t {
                None = 0,  ///< Shown, never judged.
                Zero = 1,  ///< Any increase is a problem (`Limits::health`), for `hold` s after the last.
                Above = 2, ///< Above `warn` or `crit` for `hold` s (a `Total`'s rate a second; a `Peak` rising past).
                Below = 3, ///< Below `warn` or `crit` for `hold` s.
                Drift = 4, ///< Its magnitude above `warn` or `crit` for `hold` s.
                State = 5, ///< In a state whose bit is set in `warn` or `crit`.
        };

        /** @brief How a counter is doing. */
        enum class Health : uint8_t {
                Normal = 0,
                Warning = 1,  ///< Amber.
                Critical = 2, ///< Red.
        };

        /** @brief A counter's flags. */
        enum Flag : uint8_t {
                KeepHistory = 1u << 0, ///< Kept for graphs while watched.
                Adjusted = 1u << 1,    ///< Its group sets its levels from what it knows (`Stats::Group::adjust`).
        };

        /** @brief A `Zero` or `Peak` problem's default hold, in seconds. */
        constexpr uint16_t DefaultEventHold = 60;

        /** @brief A rule and its levels. */
        struct Limits {
                        Rule     rule = Rule::None;
                        Health   health = Health::Warning; ///< `Zero`: the problem an increase is.
                        uint16_t hold = 0;                 ///< Seconds (`Zero`, `Peak`: 0 means 60).
                        int64_t  warn = NoLevel;           ///< `State`: a bit per state.
                        int64_t  crit = NoLevel;
        };

        /** @brief A counter, as its component declares it (rodata). */
        struct Desc {
                        const char *name = ""; ///< After the group's prefix and a dot: `late`.
                        Unit        unit = Unit::Count;
                        Kind        kind = Kind::Level;
                        uint8_t     flags = 0; ///< `Flag` bits.
                        Limits      limits = {};
                        const char *labels = ""; ///< `State`: the states' names, `|` between.
                        const char *help = "";   ///< One line saying what it counts.
        };

        /** @brief Returns @p unit's short name (`us`, `bytes`, ...). */
        const char *unitName(Unit unit);

        /** @brief Returns @p kind's name. */
        const char *kindName(Kind kind);

        /** @brief Returns @p rule's name (`zero`, `above`, ...). */
        const char *ruleName(Rule rule);

        /** @brief Returns @p health's name (`normal`, `warning`, `critical`). */
        const char *healthName(Health health);

        /** @brief Returns label @p index of @p labels (`a|b|c`), or empty. */
        StringView label(const char *labels, int64_t index);

        /**
         * @brief Returns what an event rule (`Zero`, or a `Peak` judged `Above`)
         *        makes of @p value as first seen: a problem that happened before
         *        anyone looked (`ProblemLog::noteEarlier()`), or `Normal`.
         */
        Health eventHealth(const Limits &limits, Kind kind, int64_t value);

        /**
         * @brief Judges one counter, sample by sample (`devplan/sysmon.md`
         *        §3.3): remembers its last value, when it went wrong, and its
         *        health.
         *
         * @par Thread Safety
         * A value with one owner.
         */
        class Judge {
                public:
                        /**
                         * @brief Takes the value read at @p nowMs (monotonic) and returns
                         *        the counter's health now. A `Total` or `Peak` that fell
                         *        was reset (its source restarted): it counts from there.
                         */
                        Health update(const Limits &limits, Kind kind, int64_t value, int64_t nowMs);

                        /** @brief Returns the health of the last `update()`. */
                        Health health() const { return _health; }

                        /**
                         * @brief Returns how much a `Total` or `Peak` rose at the last
                         *        `update()` (0 on the first, or after a reset counts all
                         *        it has since).
                         */
                        int64_t delta() const { return _delta; }

                        /** @brief Returns a `Total`'s rate a second at the last `update()` (`Unavailable` if none yet). */
                        int64_t rate() const { return _rate; }

                        /** @brief Returns true until the first `update()` with a value. */
                        bool isFirst() const { return _lastMs == NoValue; }

                        /**
                         * @brief Returns when the present problem began (monotonic ms): a
                         *        level rule's value going out of normal, an event rule's
                         *        first increase within its hold; -1 if none.
                         */
                        int64_t since() const;

                        /**
                         * @brief Stops judging (nobody watches): forgets the health, the
                         *        holds and the rate, keeping the last value, so the next
                         *        `update()` counts what came meanwhile (an event rule's
                         *        increase is a problem then) but has no rate over the gap.
                         */
                        void pause();

                        /** @brief Forgets everything (a counter reused). */
                        void reset() { *this = Judge(); }

                private:
                        Health judgeLevel(const Limits &limits, int64_t value) const;

                        static constexpr int64_t NoValue = -1; // _lastMs: none yet
                        static constexpr int64_t NoTime = -2;  // _lastMs: a value, from before a pause

                        int64_t _last = 0;
                        int64_t _lastMs = NoValue;
                        int64_t _eventStartMs = -1; // an event rule: the first increase of the problem
                        int64_t _badSinceMs = -1;   // a level rule: when the value went out of normal
                        int64_t _eventMs = -1;      // an event rule: the last increase that was a problem
                        int64_t _delta = 0;
                        int64_t _rate = Unavailable;
                        Health  _pending = Health::Normal; // a level rule: what it is while not yet held
                        Health  _eventHealth = Health::Normal;
                        Health  _health = Health::Normal;
        };

        /**
         * @brief The problems seen, as episodes (`devplan/sysmon.md` §3.3): one
         *        opens when a counter goes out of normal, gathers what follows,
         *        and closes when it is normal again. The last `Capacity` are
         *        kept; each change is numbered, so a reader asks for those after
         *        the last it saw.
         *
         * @par Thread Safety
         * A value with one owner (the registry, under its lock).
         */
        class ProblemLog {
                public:
                        /** @brief Episodes kept. */
                        static constexpr size_t Capacity = 64;

                        /** @brief An episode's flags. */
                        enum Flags : uint8_t {
                                Open = 1u << 0,    ///< Not normal again yet.
                                Earlier = 1u << 1, ///< Found on the first look: it happened before anyone watched.
                        };

                        /** @brief A problem. */
                        struct Episode {
                                        uint64_t changed = 0;             ///< The change that last touched it.
                                        uint32_t id = 0;                  ///< The counter's id.
                                        Health   health = Health::Normal; ///< The worst it reached.
                                        uint8_t  flags = 0;
                                        int64_t  startMs = 0; ///< Monotonic.
                                        int64_t  endMs = 0;   ///< When it closed (open: the last update).
                                        int64_t  firstValue = 0;
                                        int64_t  lastValue = 0;
                                        uint64_t count =
                                                0; ///< An event rule's increases; a level rule's samples out of normal.
                                        char name[MaxName + 1] = {};

                                        bool isOpen() const { return (flags & Open) != 0; }
                        };

                        explicit ProblemLog(MemSpace space = MemSpace::Psram) : _episodes(space) {}

                        /**
                         * @brief Takes a counter's verdict: @p before and @p after its
                         *        `Judge::update()`, the value, and @p count (the increases
                         *        for an event rule, else 1). Opens, widens or closes the
                         *        counter's episode; one opened starts at @p sinceMs
                         *        (`Judge::since()`; -1: now).
                         * @return `Ok`, or `NoMem` (the first problem's storage).
                         */
                        Error note(uint32_t id, StringView name, Health before, Health after, int64_t value,
                                   uint64_t count, int64_t nowMs, int64_t sinceMs = -1);

                        /**
                         * @brief Records a closed episode found on the first look (a
                         *        `Zero` counter already above zero, a `Peak` already past a
                         *        level): @p count the increases (a `Total`'s value; 1).
                         */
                        Error noteEarlier(uint32_t id, StringView name, Health health, int64_t value, uint64_t count,
                                          int64_t nowMs);

                        /** @brief Closes @p id's open episode, if any (its counter went away). */
                        void close(uint32_t id, int64_t nowMs);

                        /** @brief Closes every open episode (nobody watches any more). */
                        void closeAll(int64_t nowMs);

                        /**
                         * @brief Copies the episodes changed after @p after into @p out,
                         *        the earliest change first.
                         * @return How many were copied.
                         */
                        size_t changedAfter(uint64_t after, Span<Episode> out) const;

                        /** @brief Returns the newest change's number (0: none). */
                        uint64_t lastChange() const { return _changes; }

                        /** @brief Marks every change up to @p change as seen (the Problems list shown). */
                        void markSeen(uint64_t change) { _seen = change > _seen ? change : _seen; }

                        /**
                         * @brief Returns the worst health of the open episodes and those
                         *        changed since last seen, leaving out the `Earlier` ones.
                         */
                        Health unseen() const;

                        /** @brief Returns the episodes kept. */
                        size_t size() const { return _episodes.size(); }

                        /** @brief Forgets the closed episodes. */
                        void clearClosed();

                private:
                        Episode *openFor(uint32_t id);
                        Episode *make(); // a new one, or the oldest closed one reused

                        List<Episode> _episodes;
                        uint64_t      _changes = 0;
                        uint64_t      _seen = 0;
        };

        /**
         * @brief The overrides of `sysmon/limits` (`devplan/sysmon.md` §3.3):
         *        `PATTERN:SETTING[,SETTING];...`, where a pattern is a counter's
         *        name with `*` matching anything, and a setting is `off`,
         *        `warn` or `crit` (a `Zero` rule's problem), `rule=NAME`,
         *        `warn=N`, `crit=N` (`none`: unused) or `hold=S`. The last
         *        matching entry wins.
         *
         * @par Thread Safety
         * A value with one owner.
         */
        class Overrides {
                public:
                        explicit Overrides(MemSpace space = MemSpace::Psram) : _entries(space) {}

                        /**
                         * @brief Takes @p text, replacing what was there; entries that
                         *        can't be read are skipped.
                         * @return How many were skipped (`*bad`, if given); `Ok` or `NoMem`.
                         */
                        Error parse(StringView text, size_t *bad = nullptr);

                        /** @brief Applies the entries matching @p name to @p limits. @return True if any matched. */
                        bool apply(StringView name, Limits &limits) const;

                        /** @brief Returns the entries kept. */
                        size_t size() const { return _entries.size(); }

                        /** @brief Returns true if @p name matches @p pattern (`*`: anything). */
                        static bool matches(StringView pattern, StringView name);

                private:
                        struct Entry {
                                        char     pattern[MaxName + 1] = {};
                                        uint8_t  set = 0; // which fields it sets
                                        Rule     rule = Rule::None;
                                        Health   health = Health::Warning;
                                        uint16_t hold = 0;
                                        int64_t  warn = NoLevel;
                                        int64_t  crit = NoLevel;
                        };

                        static bool parseEntry(StringView text, Entry &out);

                        List<Entry> _entries;
        };

        /**
         * @brief The samples kept for graphs (`devplan/sysmon.md` §3.4): a
         *        number of series, each the last `depth` samples, the oldest
         *        overwritten.
         *
         * @par Thread Safety
         * A value with one owner.
         */
        class History {
                public:
                        explicit History(MemSpace space = MemSpace::Psram) : _samples(space) {}

                        /** @brief Makes room for @p series series of @p depth samples, emptied. @return `Ok` or `NoMem`. */
                        Error setup(size_t series, size_t depth);

                        /** @brief Frees the samples. */
                        void release();

                        /** @brief Adds a sample to each series (@p samples has one a series; missing ones are 0). */
                        void push(Span<const int32_t> samples);

                        /** @brief Copies series @p series's samples into @p out, the oldest first. @return How many. */
                        size_t copy(size_t series, Span<int32_t> out) const;

                        /** @brief Returns the samples each series holds. */
                        size_t count() const { return _count; }

                        size_t series() const { return _series; }
                        size_t depth() const { return _depth; }

                private:
                        List<int32_t> _samples;
                        size_t        _series = 0;
                        size_t        _depth = 0;
                        size_t        _next = 0; // where the next sample goes
                        size_t        _count = 0;
        };

} // namespace Stat

BMDS_NAMESPACE_END
