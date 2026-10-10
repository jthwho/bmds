/**
 * @file      monitor.h
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
#include <bmds/stringview.h>

#include "source.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The System Monitor's view of the counters, pure (`devplan/sysmon.md`
 *        §3.5): the catalogue (listed again when the registry's generation
 *        changes), each refresh's values and health from one read, rates from
 *        two reads, lookups by name, the worst health under a name pattern, and
 *        values as text with their units.
 *
 * @par Thread Safety
 * Not thread-safe: the window's loop.
 */
class Monitor {
        public:
                /** @brief No such counter (`find()`). */
                static constexpr size_t NotFound = SIZE_MAX;

                /** @brief Text for one value: the longest `format()` writes, with its NUL. */
                static constexpr size_t TextBytes = 32;

                explicit Monitor(Source &source) : _source(source) {}

                /**
                 * @brief Reads every counter (listing them again first if the registry
                 *        changed); a `Total`'s rate from the previous refresh.
                 * @return `Ok` or `NoMem`.
                 */
                Error refresh();

                /** @brief Returns the source. */
                Source &source() const { return _source; }

                /** @brief Returns the status read by the last `refresh()`. */
                const Source::Status &status() const { return _status; }

                /** @brief Returns the counters. */
                size_t count() const { return _entries.size(); }

                /** @brief Returns counter @p i's description. */
                const Source::Counter &counter(size_t i) const { return _entries[i].counter; }

                /** @brief Returns counter @p i's value (`Stat::Unavailable` if not measured). */
                int64_t value(size_t i) const { return _entries[i].value; }

                /** @brief Returns a `Total`'s rate a second (`Stat::Unavailable`: none yet, or not a total). */
                int64_t rate(size_t i) const { return _entries[i].rate; }

                /** @brief Returns counter @p i's health (Normal while not judged). */
                Stat::Health health(size_t i) const;

                /** @brief Returns true if counter @p i is judged now (someone watches). */
                bool isJudged(size_t i) const { return _entries[i].health != Source::Unjudged; }

                /** @brief Returns the counter named @p name's index, or `NotFound`. */
                size_t find(StringView name) const;

                /** @brief Returns the value of the counter named @p name (`Stat::Unavailable`: none). */
                int64_t valueOf(StringView name) const;

                /** @brief Returns the rate of the counter named @p name (`Stat::Unavailable`: none). */
                int64_t rateOf(StringView name) const;

                /** @brief Returns the worst health of the counters matching @p pattern (`*` anything). */
                Stat::Health worst(StringView pattern) const;

                /**
                 * @brief Writes @p value in @p unit into @p out (`TextBytes`): scaled
                 *        (`1.25 MB`, `3.4 ms`, `12.5 %`), `-` if unavailable.
                 * @return @p out.
                 */
                static const char *format(char (&out)[TextBytes], Stat::Unit unit, int64_t value);

                /** @brief Writes counter @p i's value: a state's label, else `format()`. @return @p out. */
                const char *formatValue(char (&out)[TextBytes], size_t i) const;

                /** @brief Writes counter @p i's rate with `/s` (empty: none). @return @p out. */
                const char *formatRate(char (&out)[TextBytes], size_t i) const;

        private:
                struct Entry {
                                Source::Counter counter;
                                int64_t         value = Stat::Unavailable;
                                int64_t         rate = Stat::Unavailable;
                                uint8_t         health = Source::Unjudged;
                };

                Error relist();

                Source        &_source;
                Source::Status _status;
                List<Entry>    _entries{MemSpace::Psram};
                List<uint32_t> _ids{MemSpace::Psram};
                List<int64_t>  _values{MemSpace::Psram};
                List<uint8_t>  _health{MemSpace::Psram};
                uint32_t       _generation = 0;
                int64_t        _lastNs = 0;
                bool           _listed = false;
};

BMDS_NAMESPACE_END
