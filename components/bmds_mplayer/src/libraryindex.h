/**
 * @file      libraryindex.h
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
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The library's index (`devplan/mediaplayer.md` §6f), pure: a record
 *        a file (its size and time, to see a change; its length, format and
 *        tags), kept sorted by path, read from and written to
 *        `library.idx`.
 *
 * The file: a line a record, fields separated by tabs, in this order:
 * path, size, modified (seconds), length (ms), format, title, artist, album,
 * track, year. A tab, a line break or a backslash in a text is written
 * `\t`, `\n`, `\r`, `\\`. Fields are only ever appended; a line with more
 * is read, its extra fields ignored. A first line `#bmds-library 1` names
 * the format.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class LibraryIndex {
        public:
                /** @brief Records at most. */
                static constexpr size_t MaxRecords = 50000;

                /** @brief A file's record (views: into the index, or the caller's for `set()`). */
                struct Record {
                                StringView path;
                                uint64_t   size = 0;
                                int64_t    modified = 0;
                                uint32_t   lengthMs = 0;
                                StringView format;
                                StringView title;
                                StringView artist;
                                StringView album;
                                uint16_t   track = 0;
                                uint16_t   year = 0;
                };

                explicit LibraryIndex(MemSpace space = MemSpace::Psram);

                /** @brief Returns the number of records. */
                size_t count() const { return _records.size(); }

                /** @brief Returns record @p index (in path order). */
                Record record(size_t index) const;

                /** @brief Returns @p path's record's index, or -1. */
                int find(StringView path) const;

                /**
                 * @brief Adds or replaces @p record (by its path); its views must not be
                 *        into this index.
                 * @return `Ok`, `Invalid` (no path, a text over 64 KB), `Full`, `NoMem`.
                 */
                Error set(const Record &record);

                /** @brief Removes @p path's record, if there is one. */
                void remove(StringView path);

                /** @brief Removes every record. */
                void clear();

                /**
                 * @brief Reads the records of index file @p text (adding to, or replacing,
                 *        those it has).
                 * @return The lines it couldn't read (skipped), or `NoMem` / `Full`.
                 */
                Result<size_t> read(StringView text);

                /** @brief Writes the index file into @p out (replaced). @return `Ok` or `NoMem`. */
                Error write(String &out) const;

        private:
                struct Text {
                                uint32_t at = 0;
                                uint16_t length = 0;
                };

                struct Stored {
                                Text     path;
                                Text     format;
                                Text     title;
                                Text     artist;
                                Text     album;
                                uint64_t size = 0;
                                int64_t  modified = 0;
                                uint32_t lengthMs = 0;
                                uint16_t track = 0;
                                uint16_t year = 0;
                };

                StringView view(const Text &t) const { return StringView(_texts.data() + t.at, t.length); }
                Error      store(StringView s, Text &out);
                void       release(const Stored &s);
                void       compact();
                size_t     lowerBound(StringView path) const;

                List<Stored> _records;
                List<char>   _texts;
                size_t       _unused = 0;
};

BMDS_NAMESPACE_END
