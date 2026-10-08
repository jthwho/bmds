/**
 * @file      copiedfiles.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The files a file manager put on the clipboard, copied or cut
 *        (`devplan/filebrowser.md` §4j, FB11), and the clipboard's lists of
 *        files read back.
 *
 * Paths are absolute, a line each. They are offered as `text/uri-list`, as
 * `x-special/gnome-copied-files` (`copy` or `cut` on the first line, then a
 * `file://` URI a line: GNOME's format, which other file managers read too),
 * and as plain text.
 *
 * @par Thread Safety
 * Not thread-safe: one owner (the panel's loop). `parse()` is reentrant.
 */
class CopiedFiles {
        public:
                /** @brief A list of URIs (RFC 2483). */
                static constexpr StringView UriList{"text/uri-list"};

                /** @brief GNOME's copied or cut files. */
                static constexpr StringView GnomeFiles{"x-special/gnome-copied-files"};

                /**
                 * @brief Keeps @p paths (a line each), copied or @p cut.
                 * @return `Ok`, or `NoMem` (then empty).
                 */
                Error set(StringView paths, bool cut);

                /** @brief Empties it. */
                void clear();

                /** @brief Returns true if it holds no paths. */
                bool isEmpty() const { return _paths.isEmpty(); }

                /** @brief Returns true if the paths were cut. */
                bool isCut() const { return _cut; }

                /** @brief Returns true if @p path is one of the paths, and they were cut. */
                bool isCut(StringView path) const;

                /**
                 * @brief Returns true if @p name in folder @p folder is one of the paths,
                 *        and they were cut: no path is put together (a view asks as it
                 *        draws, on a small stack).
                 */
                bool isCut(StringView folder, StringView name) const;

                /** @brief Returns the paths, a line each. */
                StringView paths() const { return _paths.view(); }

                /**
                 * @brief Writes the paths as @p type into @p out: a URI list, GNOME's
                 *        list, or else plain text.
                 * @return `Ok` or `NoMem`.
                 */
                Error render(StringView type, String &out) const;

                /**
                 * @brief Reads a list of files: @p data as @p type (a URI list, or
                 *        GNOME's), each `file://` URI's path decoded into @p paths, a
                 *        line each. Other URIs and comments are left out.
                 * @param cut [out] True if GNOME's list says `cut`.
                 * @return `Ok`, `Empty` (no file in it), or `NoMem`.
                 */
                static Error parse(StringView type, StringView data, bool &cut, String &paths);

        private:
                String           _paths{MemSpace::Psram};
                List<StringView> _sorted{MemSpace::Psram}; // into _paths, when cut
                bool             _cut = false;
};

BMDS_NAMESPACE_END
