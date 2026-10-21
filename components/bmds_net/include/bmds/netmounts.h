/**
 * @file      netmounts.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class ProgramLauncher;

/**
 * @brief The network filesystems kept mounted from the settings
 *        (`devplan/netfs.md` step 4): `net/mounts` lists them, each a mount
 *        program and its arguments (`sshfs jth@pc:src /net/pc`), `;`-separated.
 *        A kernel thread, there while the list has entries, runs each in the
 *        foreground once an interface is up with an address, its output to
 *        the log, and starts it again when it ends (the connection dropped)
 *        after 5 s, doubling to 5 minutes. A change of the list stops the
 *        entries gone or changed and starts the new ones.
 *
 * @par Thread Safety
 * `init()` once, at boot; `parse()` and `check()` from any thread.
 */
namespace NetMounts {

        /** @brief Entries at most, and words in one (the program's name included). */
        constexpr size_t MaxEntries = 4;
        constexpr size_t MaxWords = 16;

        /** @brief One entry: views into the value parsed. */
        struct Entry {
                        StringView text;            ///< The entry, trimmed.
                        StringView words[MaxWords]; ///< The program's name, then its arguments.
                        size_t     count = 0;

                        /** @brief Where it mounts: its last word. */
                        StringView mountPoint() const { return words[count - 1]; }
        };

        /**
         * @brief Splits a value of `net/mounts` into @p entries (empty ones skipped).
         * @return `Ok`; `Invalid` (an entry not a known mount program's, `sshfs`, with
         *         arguments ending in an absolute mount point; two at one point);
         *         `Full` (more than @p entries or `MaxWords`).
         */
        Error parse(StringView value, Span<Entry> entries, size_t &count);

        /** @brief `net/mounts`' check: `parse()`'s errors. */
        Error check(StringView value);

        /**
         * @brief Declares `net/mounts`, watches it and the network, and starts the
         *        thread if it has entries; programs run through @p launcher (kept).
         *        After the network stack and the program loader.
         * @return `Ok`, or the error declaring.
         */
        Error init(ProgramLauncher &launcher);

} // namespace NetMounts

BMDS_NAMESPACE_END
