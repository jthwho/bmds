/**
 * @file      patternfiles.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The desktop patterns' files (`devplan/wallpaper.md` §4a-§4c): `NAME.png` found first
 *        on `panel/desktop/patterns`, the names there, and the saved palettes' text
 *        (`panel/desktop/palettes`), for the panel's desktop and Settings.
 *
 * @par Thread Safety
 * Reentrant (the files are the caller's to share).
 */
namespace PatternFiles {

        /** @brief The settings (the panel's, `devplan/wallpaper.md` §4a). */
        inline constexpr const char *WallpaperKey = "panel/desktop/wallpaper";
        inline constexpr const char *PatternKey = "panel/desktop/pattern";
        inline constexpr const char *ColorsKey = "panel/desktop/pattern-colors";
        inline constexpr const char *ScaleKey = "panel/desktop/pattern-scale";
        inline constexpr const char *PathsKey = "panel/desktop/patterns";
        inline constexpr const char *PalettesKey = "panel/desktop/palettes";

        /** @brief A pattern's file read at most. */
        inline constexpr size_t MaxFileBytes = 512 * 1024;

        /** @brief The saved palettes' text read at most. */
        inline constexpr size_t MaxTextBytes = 64 * 1024;

        /** @brief A file's bytes in PSRAM, owned. Move-only. */
        class Bytes {
                public:
                        Bytes() = default;
                        ~Bytes() { reset(); }
                        Bytes(Bytes &&other) noexcept
                            : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)) {}
                        Bytes(const Bytes &) = delete;
                        Bytes &operator=(const Bytes &) = delete;

                        /** @brief Reads @p path whole (at most `MaxFileBytes`). */
                        Error read(StringView path);

                        /** @brief Frees them. */
                        void reset();

                        /** @brief Returns them. */
                        ByteSpan bytes() const { return ByteSpan(_data, _size); }

                private:
                        uint8_t *_data = nullptr;
                        size_t   _size = 0;
        };

        /**
         * @brief Finds @p name's file (`NAME.png`) first in @p paths' directories (`;` between),
         *        into @p path.
         * @return `Ok`, `NotExist`, `Invalid` (an empty name, or one with `/`), or `NoMem`.
         */
        Error find(StringView paths, StringView name, String &path);

        /**
         * @brief Finds and reads @p name's file on `panel/desktop/patterns` into @p out (its path
         *        into @p path if given).
         * @return `Ok`, `find()`'s errors, `OutOfRange` (too large), or the file's.
         */
        Error load(StringView name, Bytes &out, String *path = nullptr);

        /** @brief Told of a pattern: its name (without `.png`) and its file. */
        using Found = void (*)(void *arg, StringView name, StringView path);

        /**
         * @brief Tells @p fn of each pattern in @p paths' directories, in their order, a name
         *        only the first time (the file that `find()` finds). Missing directories are
         *        passed over.
         * @return `Ok` or `NoMem`.
         */
        Error list(StringView paths, Found fn, void *arg);

        /**
         * @brief Reads @p path's text (at most `MaxTextBytes`) into @p out: empty if there is
         *        no such file.
         * @return `Ok`, `OutOfRange` (too large), `NoMem`, or the file's error.
         */
        Error readText(StringView path, String &out);

        /**
         * @brief Writes @p text as @p path whole: its folders made, written beside it and
         *        renamed over it, so a reader sees the old text or the new.
         * @return `Ok`, or the filesystem's error.
         */
        Error writeText(StringView path, StringView text);

        /** @brief Makes folder @p path and those above it that are missing. @return `Ok`, or why not. */
        Error makeFolders(StringView path);

} // namespace PatternFiles

BMDS_NAMESPACE_END
