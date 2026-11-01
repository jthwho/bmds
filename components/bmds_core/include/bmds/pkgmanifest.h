/**
 * @file      pkgmanifest.h
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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A package's manifest (devplan/packages.md §3): UTF-8 text, a line per entry, its
 *        columns separated by tabs, each line ending in a newline (the last may not). An on-disk
 *        format: append only.
 *
 * - `f PATH MODE SIZE BLAKE2S`: a file: its mode in octal (at most 7777), its size in decimal,
 *   the BLAKE2s-256 of its bytes in lowercase hex. Its bytes are next in the data section.
 * - `d PATH`: a directory.
 * - `l PATH TARGET`: a symbolic link to TARGET.
 * - `KEY VALUE`: a field (its value without tabs), KEY 2 to 32 lowercase letters, digits and `-`: `depends`
 *   (`PkgDepends`), `title` (the name people see: "Lucky Duck"), `summary` (one line),
 *   `category` (freedesktop.org's main categories, as the launcher's: `Game`, `Utility`...),
 *   `license` (SPDX), `homepage` (a URL), `stack`, `display`. Fields a reader doesn't know are skipped;
 *   an entry kind it doesn't know (one letter) is a newer format, refused.
 *
 * Paths are relative to the install root (`isValidPath()`): a manifest can't reach outside it.
 *
 * @par Thread Safety
 * A `Reader` is used by one thread at a time; the functions are pure.
 */
namespace PkgManifest {

        /** @brief The longest path, and the longest component (FAT's longest name). */
        constexpr size_t MaxPathBytes = 512;
        constexpr size_t MaxNameBytes = 255; ///< See `MaxPathBytes`.

        /** @brief Bytes of a file's hash (BLAKE2s-256). */
        constexpr size_t HashBytes = 32;

        /** @brief What a line is. */
        enum class Kind : uint8_t {
                Field,     ///< `key` and `value`.
                File,      ///< `path`, `mode`, `size`, `hash`.
                Directory, ///< `path`.
                Link,      ///< `path` and `target`.
        };

        /** @brief One line, read. Views into the manifest's text. */
        struct Entry {
                        Kind       kind = Kind::Field;
                        StringView key;
                        StringView value;
                        StringView path;
                        StringView target;
                        uint32_t   mode = 0;
                        uint64_t   size = 0;
                        uint8_t    hash[HashBytes] = {};
        };

        /**
         * @brief Returns true if @p path is a path a manifest may name: relative, its components
         *        separated by single `/`, none empty, `.` or `..`, nor longer than `MaxNameBytes`;
         *        valid UTF-8 without control characters or any of `" * : < > ? \ |` (which FAT
         *        can't hold); at most `MaxPathBytes`.
         */
        bool isValidPath(StringView path);

        /** @brief Returns true if @p text is valid UTF-8 without control characters (C0, DEL, C1). */
        bool isCleanText(StringView text);

        /** @brief Reads a manifest's lines in turn. */
        class Reader {
                public:
                        /** @brief A reader of @p text, which must outlive it. */
                        explicit Reader(StringView text) : _text(text) {}

                        /**
                         * @brief Reads the next line into @p out.
                         * @return `Ok`; `EndOfFile` after the last; `CorruptData` for a line
                         *         that isn't one (`line()` says which); `NotSupported` for an
                         *         entry kind this code doesn't know.
                         */
                        Error next(Entry &out);

                        /** @brief The line `next()` last read, from 1. */
                        size_t line() const { return _line; }

                private:
                        StringView _text;
                        size_t     _pos = 0;
                        size_t     _line = 0;
        };

} // namespace PkgManifest

/**
 * @brief A package's dependencies (devplan/packages.md §5): a comma-separated list of `NAME` or
 *        `NAME (>= VERSION)`, spaces allowed around each part. An empty list has none.
 *
 * @par Thread Safety
 * A `Reader` is used by one thread at a time.
 */
namespace PkgDepends {

        /** @brief One dependency. Views into the list's text. */
        struct Dependency {
                        StringView name;
                        StringView minVersion; ///< Empty: any version.
        };

        /** @brief Reads a list's dependencies in turn. */
        class Reader {
                public:
                        /** @brief A reader of @p list, which must outlive it. */
                        explicit Reader(StringView list) : _list(list) {}

                        /**
                         * @brief Reads the next dependency into @p out.
                         * @return `Ok`; `EndOfFile` after the last; `CorruptData` for a list that
                         *         isn't one (a bad name or version, an operator other than `>=`).
                         */
                        Error next(Dependency &out);

                private:
                        StringView _list;
                        size_t     _pos = 0;
                        bool       _done = false;
        };

} // namespace PkgDepends

BMDS_NAMESPACE_END
