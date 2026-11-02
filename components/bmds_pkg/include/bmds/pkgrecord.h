/**
 * @file      pkgrecord.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One line of `pkg --json` (devplan/packages.md §9), read back: what the first-run window
 *        and the store know of `pkg`'s output.
 *
 * Every record type and field `pkg` writes is append-only, so this reads what it knows and skips
 * the rest: an unknown type is `Type::Other`, an unknown field is passed over. A field a record
 * doesn't have stays empty, zero or false. `name` holds a record's package whichever key names it
 * (`name` or `package`).
 *
 * @par Thread Safety
 * A value: one thread at a time.
 */
struct PkgRecord {
                /** @brief The record's `type`. */
                enum class Type : uint8_t {
                        Other,       ///< A type this reader doesn't know.
                        Package,     ///< `list`, `info`, `search`.
                        Description, ///< `info`: the long description.
                        File,        ///< `info --files`, `verify`.
                        Plan,        ///< What `install`, `remove` or `upgrade` will do.
                        Progress,    ///< A step of a package's work.
                        Done,        ///< A package's work ended.
                        Note,        ///< Something to tell, not an error.
                        Source,      ///< `update`: a source's list.
                        Error,       ///< A failure.
                };

                Type     type = Type::Other;
                String   name;              ///< The package (`name` or `package`).
                String   version;           ///< `version`.
                String   from;              ///< `plan`: the version replaced.
                String   title;             ///< `package`.
                String   summary;           ///< `package`.
                String   category;          ///< `package`.
                String   depends;           ///< `package`: ", "-separated.
                String   available;         ///< `package`: the newest in the lists.
                String   signer;            ///< `package`, `plan`: "trusted", "dev", "added".
                String   action;            ///< `plan`, `done`: "install", "remove", "would install"...
                String   step;              ///< `progress`: "checking", "fetching"...
                String   source;            ///< `source`.
                String   error;             ///< `error`, `source`: `Error`'s name.
                String   text;              ///< `error`, `note`, `description`, `source`: the words.
                uint64_t size = 0;          ///< `package`: the `.bmpk`'s bytes.
                uint64_t installedSize = 0; ///< `package`, `plan`.
                uint64_t fetch = 0;         ///< `plan`: bytes to download.
                uint64_t done = 0;          ///< `progress`.
                uint64_t total = 0;         ///< `progress`.
                uint64_t packages = 0;      ///< `source`.
                bool     installed = false;
                bool     automatic = false;
                bool     upgradable = false;

                /** @brief Empties every field. */
                void clear();

                /**
                 * @brief Reads one line of `pkg --json` (without its newline) into this record.
                 * @return `Ok`; `CorruptData` (not one JSON object); `BufferTooSmall` (a string over
                 *         `MaxString` bytes); `NoMem`.
                 */
                Error parse(StringView line);

                /** @brief The longest string read (longer is `BufferTooSmall`). */
                static constexpr size_t MaxString = 2047;
};

BMDS_NAMESPACE_END
