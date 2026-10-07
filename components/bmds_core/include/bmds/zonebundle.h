/**
 * @file      zonebundle.h
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
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/timezone.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads `zoneinfo.bin`, the time zone database as one file
 *        (devplan/time.md §9; built by `tools/tzbundle`).
 *
 * Little-endian. A 32-byte header (`BMTZ`, format 1, the header's size, the
 * tzdata version, the entry count, the index's offset, the entry size, the
 * file's size); an index sorted by name, each entry the name (NUL-padded)
 * then the data's offset and length; then the TZif files, an alias sharing
 * its zone's data. Nothing is kept in memory but the header: every lookup
 * reads the index through a `Source`.
 *
 * @par Thread Safety
 * A value: `const` methods read only through the `Source` they are given.
 */
class ZoneBundle {
        public:
                /** @brief Where the bundle's bytes come from (a file). */
                class Source {
                        public:
                                /**
                                 * @brief Reads exactly `buf.size()` bytes at @p offset.
                                 * @return `Ok`, `CorruptData` if the file ends first, or the read's error.
                                 */
                                virtual Error readAt(uint32_t offset, WritableByteSpan buf) = 0;

                        protected:
                                ~Source() = default;
                };

                /** @brief Longest name kept (a longer one in a bundle is refused). */
                static constexpr size_t MaxName = 63;

                /** @brief Longest tzdata version ("2026e"). */
                static constexpr size_t MaxVersion = 8;

                /** @brief The header's size in format 1. */
                static constexpr size_t HeaderBytes = 32;

                /** @brief One index entry. */
                struct Entry {
                                char     name[MaxName + 1] = {};
                                uint32_t offset = 0; ///< Of the TZif data, in the bundle.
                                uint32_t length = 0; ///< Of the TZif data.

                                /** @brief Returns the name. */
                                StringView nameView() const { return StringView(name); }
                };

                /** @brief An empty bundle (no zones). */
                constexpr ZoneBundle() = default;

                /**
                 * @brief Reads and checks the header of the bundle in @p source.
                 * @param fileSize The file's size (a copy cut short is refused).
                 * @return The bundle, `CorruptData` (not a bundle, or damaged),
                 *         `NotSupported` (a later format), or the read's error.
                 */
                static Result<ZoneBundle> open(Source &source, uint64_t fileSize);

                /** @brief Returns the number of names (aliases included). */
                uint32_t count() const { return _count; }

                /** @brief Returns the tzdata version ("2026e"). */
                StringView version() const { return StringView(_version); }

                /**
                 * @brief Reads entry @p index (less than `count()`).
                 * @return `Ok`, `CorruptData` (a name not terminated, data
                 *         outside the file or not TZif), or the read's error.
                 */
                Error entry(Source &source, uint32_t index, Entry &out) const;

                /**
                 * @brief Returns the index of the first name not less than @p key
                 *        (`count()` if none), by binary search.
                 */
                Result<uint32_t> lowerBound(Source &source, StringView key) const;

                /**
                 * @brief Finds the zone named @p name.
                 * @return `Ok` with @p out filled, `NotExist`, or an error reading.
                 */
                Error find(Source &source, StringView name, Entry &out) const;

                /**
                 * @brief Returns the current rules of the zone in @p entry: its
                 *        TZif file's footer, a POSIX `TZ` string.
                 * @return The rules, `NotSupported` (a version 1 file, or no
                 *         footer), `CorruptData`, or an error reading.
                 */
                static Result<TimeZone> rules(Source &source, const Entry &entry);

        private:
                char     _version[MaxVersion + 1] = {};
                uint32_t _count = 0;
                uint32_t _indexOffset = 0;
                uint32_t _entryBytes = 0;
                uint32_t _dataStart = 0; // The index's end.
                uint32_t _fileSize = 0;
};

BMDS_NAMESPACE_END
