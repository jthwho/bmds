/**
 * @file      leapseconds.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief IANA's leap-second list (`leap-seconds.list`, in tzdata; devplan/network.md §31e): when
 *        TAI less UTC changed, when the list was made and when it expires.
 *
 * The file's lines are NTP seconds (from 1900) and the offset from then on, `#$` its update,
 * `#@` its expiry and `#h` a SHA-1 over the digits of the update, the expiry and each line's
 * two numbers, run together. A list must have all three and a matching hash.
 *
 * @par Thread Safety
 * Pure functions over a plain table.
 */
namespace LeapSeconds {

        /** @brief Changes a table holds (28 in 2026). */
        constexpr size_t MaxEntries = 64;

        /** @brief NTP's seconds at 1970-01-01. */
        constexpr int64_t NtpToUnix = 2'208'988'800;

        /** @brief One change: from @p at (seconds since 1970, UTC) TAI is UTC plus @p offset. */
        struct Entry {
                        int64_t at = 0;
                        int16_t offset = 0;
        };

        /** @brief A parsed list. */
        struct Table {
                        Entry    entries[MaxEntries];
                        uint16_t count = 0;
                        int64_t  updated = 0; ///< Seconds since 1970 (`#$`).
                        int64_t  expires = 0; ///< Seconds since 1970 (`#@`).
        };

        /** @brief What `parse()` made of a file. */
        enum class Verdict : uint8_t {
                Ok,
                Malformed, ///< Not a leap-second list: a bad line, a missing `#$`, `#@` or `#h`, times out of order.
                BadHash,   ///< Its hash doesn't match: changed, or cut short.
                TooMany,   ///< More than `MaxEntries` changes.
        };

        /** @brief Returns what @p v means, as a phrase ("its hash doesn't match"). */
        const char *verdictText(Verdict v);

        /** @brief Reads @p text into @p out; @p out is only valid for `Ok`. */
        Verdict parse(StringView text, Table &out);

        /** @brief Returns TAI less UTC at @p unixSeconds (0 before the first change). */
        int16_t offsetAt(const Table &table, int64_t unixSeconds);

        /** @brief Returns the first change after @p unixSeconds, or null. */
        const Entry *nextAfter(const Table &table, int64_t unixSeconds);

} // namespace LeapSeconds

BMDS_NAMESPACE_END
