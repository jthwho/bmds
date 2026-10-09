/**
 * @file      m3u.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

#include "playlist.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief M3U playlists (`devplan/mediaplayer.md` §6f), pure: plain and
 *        extended (`#EXTM3U`, `#EXTINF`), `.m3u` and `.m3u8` alike (UTF-8).
 *
 * Reading takes LF or CR LF, a byte order mark, comments, blank lines;
 * backslashes as separators; paths relative to the list's folder. URLs are
 * skipped (no network). `#EXTINF`'s length and "Artist - Title" become the
 * entry's information, marked not known (the file's own tags replace it).
 *
 * Writing gives `#EXTM3U`, an `#EXTINF` line for each entry with a length or
 * a title, and paths relative to the list's folder when they are under it.
 */
namespace M3u {

        /** @brief What `read()` did. */
        struct ReadResult {
                        size_t added = 0;   ///< Entries added.
                        size_t skipped = 0; ///< Lines naming no file here (URLs, bad paths).
        };

        /**
         * @brief Adds the entries of list @p text, which is in folder @p folder
         *        (absolute), at the end of @p playlist.
         * @return What it did, or `NoMem` / `Full` (the entries before it stay).
         */
        Result<ReadResult> read(StringView text, StringView folder, Playlist &playlist);

        /**
         * @brief Writes @p playlist as an extended M3U kept in folder @p folder
         *        (absolute) into @p out (replaced).
         * @return `Ok` or `NoMem`.
         */
        Error write(const Playlist &playlist, StringView folder, String &out);

} // namespace M3u

BMDS_NAMESPACE_END
