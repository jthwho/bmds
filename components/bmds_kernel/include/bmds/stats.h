/**
 * @file      stats.h
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
#include <bmds/span.h>
#include <bmds/stat.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's counters in one registry (`devplan/sysmon.md` §3.2-
 *        §3.4): each component registers a `Group` of named counters, read
 *        through one function; the registry judges them by their rules (and
 *        `sysmon/limits`' overrides), logs the problems, and keeps history,
 *        all only while someone watches.
 *
 * A counter's id is its group's serial number (16 bits) and its index in the
 * group (8 bits): it is never reused while its group is registered, and a
 * group registered later gets a new serial.
 *
 * **Lock order:** the registry's lock is held while it calls a group's read
 * function, which takes its component's locks; so a component must not add or
 * remove a group while holding a lock its read function takes.
 *
 * @par Thread Safety
 * Thread-safe (one lock). `init()` before the first `add()` (the kernel's boot
 * stage, or a test).
 */
namespace Stats {

        /** @brief A group's counters at most. */
        constexpr size_t MaxPerGroup = 64;

        /** @brief Samples kept for each counter with `Stat::KeepHistory`. */
        constexpr size_t HistoryDepth = 120;

        /** @brief The sampling interval unless set (`sysmon/interval`). */
        constexpr uint32_t DefaultIntervalMs = 1000;

        /** @brief No counter. */
        constexpr uint32_t NoId = 0;

        /** @brief A group's longest name (`audio.dsp0`). */
        constexpr size_t MaxGroupName = 23;

        /**
         * @brief A component's counters: `Stat::Desc`s in rodata, a name
         *        (`audio.dsp0`) and a function reading them all at once.
         *        Owned by the component, embedded (a few words: what the
         *        registry keeps for it is allocated in PSRAM when it is added).
         *
         * @par Thread Safety
         * Set up before `add()`; not changed while added.
         */
        class Group {
                public:
                        /** @brief Reads every counter into @p values (one each, in order; `Stat::Unavailable` if not now). */
                        using Read = void (*)(void *arg, Span<int64_t> values);

                        /** @brief Sets counter @p index's levels from what the component knows (`Stat::Adjusted`). */
                        using Adjust = void (*)(void *arg, size_t index, Stat::Limits &limits);

                        Group(const Stat::Desc *descs, size_t count, Read read, void *arg, Adjust adjust = nullptr)
                            : _descs(descs), _read(read), _adjust(adjust), _arg(arg),
                              _count(uint8_t(count <= MaxPerGroup ? count : 0)) {}

                        /** @brief Removed first if added. */
                        ~Group();
                        Group(const Group &) = delete;
                        Group &operator=(const Group &) = delete;

                        /**
                         * @brief Names it @p base, with @p number after it unless negative
                         *        (`audio.dsp` 0: `audio.dsp0`), cut at `MaxGroupName`.
                         */
                        void setName(StringView base, int number = -1);

                        /** @brief Returns its name. */
                        StringView name() const;

                        /** @brief Returns true while registered. */
                        bool isAdded() const { return _serial != 0; }

                        /** @brief Returns counter @p index's id (`NoId` if not added). */
                        uint32_t idOf(size_t index) const {
                                return _serial == 0 ? NoId : (uint32_t(_serial) << 8) | index;
                        }

                private:
                        friend struct Registry;
                        friend Error    add(Group &group);
                        friend void     remove(Group &group);
                        friend Error    info(size_t index, struct Info &out);
                        friend uint32_t find(StringView name);
                        friend size_t   count();
                        friend Error    watch();
                        friend size_t   history(uint32_t id, Span<int32_t> out);

                        const Stat::Desc *_descs;
                        Read              _read;
                        Adjust            _adjust;
                        void             *_arg;
                        Group            *_next = nullptr;
                        struct State     *_state = nullptr; // the registry's, in PSRAM, once sampled
                        uint16_t          _serial = 0;
                        uint8_t           _count;
                        char              _name[MaxGroupName + 1] = {};
        };

        /** @brief A counter as listed. */
        struct Info {
                        uint32_t     id = NoId;
                        char         name[Stat::MaxName + 1] = {};
                        Stat::Desc   desc;   ///< As declared (`desc.name` is the part after the group's).
                        Stat::Limits limits; ///< In force: adjusted, then overridden.
                        Stat::Health health = Stat::Health::Normal; ///< At the last sample (while `judged`).
                        int64_t      value = Stat::Unavailable;     ///< Read now.
                        int64_t      rate =
                                Stat::Unavailable; ///< A `Total`'s a second, at the last sample (while `judged`).
                        bool     judged = false;   ///< Watched: the sampler runs the rules.
                        uint32_t generation = 0;   ///< The registry's (`generation()`).
        };

        /** @brief Sets the registry up (no allocation). @return `Ok`. Idempotent. */
        Error init();

        /**
         * @brief Registers @p group (no allocation: its judges are made in PSRAM
         *        when it is first sampled).
         * @return `Ok`, `Exists` (already), `Invalid` (no name, no counters, or
         *         too many), or `InvalidState` (not set up).
         */
        Error add(Group &group);

        /** @brief Unregisters @p group; its open problems close. No effect if not added. */
        void remove(Group &group);

        /** @brief Returns a number that changes whenever a group is added or removed. */
        uint32_t generation();

        /** @brief Returns the counters registered. */
        size_t count();

        /**
         * @brief Describes the counter at @p index (in registration order, the
         *        groups' counters in theirs), its value read now.
         * @return `Ok`, or `NotFound` past the last.
         */
        Error info(size_t index, Info &out);

        /** @brief Returns the id of the counter named @p name (`NoId`: none). */
        uint32_t find(StringView name);

        /**
         * @brief Reads the counters @p ids at one moment into @p values (as many
         *        as both hold; `Stat::Unavailable` for an id gone).
         * @return How many were read.
         */
        size_t read(Span<const uint32_t> ids, Span<int64_t> values);

        /**
         * @brief Starts watching: the first watcher starts the sampler (each
         *        interval: every counter read and judged, history kept).
         * @return `Ok`, or `NoMem` (the sampler couldn't start).
         */
        Error watch();

        /** @brief Stops watching: the last watcher stops the sampler and frees the history. */
        void unwatch();

        /** @brief Returns the watchers. */
        size_t watchers();

        /** @brief Sets the sampling interval (100 ms to 60 s), from the next sample. */
        void setInterval(uint32_t ms);

        /** @brief Returns the sampling interval. */
        uint32_t interval();

        /**
         * @brief Samples now: reads and judges every counter, notes problems,
         *        keeps history if watched. The sampler calls it; so may a test.
         */
        void sample(int64_t nowMs);

        /**
         * @brief Copies counter @p id's history into @p out, the oldest first:
         *        a `Total`'s increase each interval, else its value (clamped to
         *        32 bits).
         * @return How many (0: none kept, or not watched).
         */
        size_t history(uint32_t id, Span<int32_t> out);

        /** @brief Copies the problems changed after @p after into @p out (`Stat::ProblemLog::changedAfter()`). */
        size_t problems(uint64_t after, Span<Stat::ProblemLog::Episode> out);

        /** @brief Returns the newest problem change's number. */
        uint64_t lastProblemChange();

        /** @brief Marks the problems up to @p change as seen. */
        void markProblemsSeen(uint64_t change);

        /** @brief Returns the worst health of the open problems and those not yet seen. */
        Stat::Health unseenProblems();

        /** @brief Forgets the closed problems. */
        void clearProblems();

        /**
         * @brief Takes `sysmon/limits` (`Stat::Overrides`), applied from the next
         *        sample and listing.
         * @return `Ok` or `NoMem`; @p bad (if given) the entries skipped.
         */
        Error setOverrides(StringView text, size_t *bad = nullptr);

} // namespace Stats

BMDS_NAMESPACE_END
