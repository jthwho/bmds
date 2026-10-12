/**
 * @file      logtext.h
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
 * @brief The text forms of the system log (devplan/logging.md): syslog
 *        messages as programs send them to `/dev/log`, and the kernel's
 *        lines as the log files take them.
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace LogText {

        /** @brief The longest tag taken from a message (`ident` or `ident[pid]`). */
        inline constexpr size_t MaxTag = 48;

        /** @brief syslog's severity when a message has no `<PRI>`: info. */
        inline constexpr uint8_t DefaultSeverity = 6;

        /** @brief A syslog message, read by `parseSyslog()`; the views point into it. */
        struct Syslog {
                        uint8_t    severity = DefaultSeverity; ///< 0 (emerg) to 7 (debug).
                        bool       hasPriority = false;        ///< A valid `<PRI>` was there.
                        StringView tag;                        ///< `ident` or `ident[pid]`; empty if none.
                        StringView text;                       ///< The rest, line ends and all.
        };

        /**
         * @brief Reads one message as RFC 3164 (and musl's `syslog()`) has it:
         *        an optional `<PRI>` (at most 191), an optional timestamp
         *        `Mmm dd hh:mm:ss ` (skipped), an optional tag ending in `:`
         *        (no spaces, at most `MaxTag`), and the text. A part that
         *        doesn't match is taken as text.
         */
        Syslog parseSyslog(StringView message);

        /**
         * @brief Reads a priority as `logger -p` takes it: a number (at most 191),
         *        or `LEVEL` or `FACILITY.LEVEL` by syslog's names (`user.notice`,
         *        `daemon.err`, `local3.debug`; `warn`, `error` and `panic` as their
         *        synonyms). The facility is checked, not kept.
         * @return True with @p severity set, or false if it isn't one.
         */
        bool parsePriority(StringView text, uint8_t &severity);

        /**
         * @brief Takes the next line from @p rest (up to `\n`, which goes; a `\r`
         *        before it too), leaving the remainder in @p rest.
         */
        StringView nextLine(StringView &rest);

        /**
         * @brief Copies @p line into @p out (at most @p capacity bytes) as a log
         *        line shows it: a tab as a space, other control characters and
         *        DEL as `?`. UTF-8 passes unchanged.
         * @return The bytes copied.
         */
        size_t cleanLine(StringView line, char *out, size_t capacity);

        /**
         * @brief Reads the uptime stamp a kernel log line starts with,
         *        `[  123.456] `.
         * @param line The line.
         * @param uptimeMs Set to the stamp, in milliseconds.
         * @param stampBytes Set to the stamp's length, its space included.
         * @return True if the line starts with one.
         */
        bool parseStamp(StringView line, uint64_t &uptimeMs, size_t &stampBytes);

} // namespace LogText

BMDS_NAMESPACE_END
