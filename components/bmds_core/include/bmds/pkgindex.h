/**
 * @file      pkgindex.h
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
 * @brief A package index's stanzas (devplan/packages.md §8), as Debian's `Packages` file: UTF-8
 *        text, a stanza per package, `Field: value` lines, stanzas separated by empty lines.
 *        An on-disk format: append only.
 *
 * Every stanza has `Package`, `Version`, `Architecture`, `ABI`, `Size`, `BLAKE2s` and `File`;
 * it may have `Installed-Size`, `Depends` (`PkgDepends`), `Title`, `Summary`, `Category` (1 to
 * 32 letters, digits and `-`), `License`, `Homepage` and `Description` (one line). Fields a
 * reader doesn't know are skipped; a field twice in a stanza is refused. `File` and `Homepage`
 * are URLs (`File` may be a path relative to the index's): printable ASCII without spaces.
 *
 * @par Thread Safety
 * A `Reader` is used by one thread at a time.
 */
namespace PkgIndex {

        /** @brief Bytes of a package's hash (BLAKE2s-256 of the whole `.bmpk`). */
        constexpr size_t HashBytes = 32;

        /** @brief The longest `File`. */
        constexpr size_t MaxFileBytes = 1024;

        /** @brief One stanza. Views into the index's text. */
        struct Stanza {
                        StringView package;
                        StringView version;
                        StringView architecture;
                        StringView depends; ///< Empty: none.
                        StringView title;
                        StringView summary;
                        StringView category;
                        StringView license;
                        StringView homepage;
                        StringView file;
                        StringView description;
                        uint32_t   abi = 0;
                        uint64_t   size = 0;
                        uint64_t   installedSize = 0;
                        uint8_t    hash[HashBytes] = {};
        };

        /** @brief Reads an index's stanzas in turn. */
        class Reader {
                public:
                        /** @brief A reader of @p text, which must outlive it. */
                        explicit Reader(StringView text) : _text(text) {}

                        /**
                         * @brief Reads the next stanza into @p out.
                         * @return `Ok`; `EndOfFile` after the last; `CorruptData` for a stanza
                         *         that isn't one (`line()` says where): a bad line or value, a
                         *         field missing or twice.
                         */
                        Error next(Stanza &out);

                        /** @brief The line `next()` last read, from 1. */
                        size_t line() const { return _line; }

                private:
                        StringView _text;
                        size_t     _pos = 0;
                        size_t     _line = 0;
        };

} // namespace PkgIndex

BMDS_NAMESPACE_END
