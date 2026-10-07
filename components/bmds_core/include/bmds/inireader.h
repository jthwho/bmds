/**
 * @file      inireader.h
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
 * @brief Walks an INI text: the configuration files' syntax
 *        (`devplan/gui.md` §4d), the freedesktop.org Desktop Entry files'
 *        among them.
 *
 * Lines end in LF (a CR before it is dropped). Each is a group (`[Name]`), an
 * entry (`key=value`, spaces around both trimmed), a comment (`#` or `;`
 * first), blank, or invalid (reported for the caller to complain about and
 * skip). Values are the text as written: `unescape()` applies the Desktop
 * Entry escapes, `nextListItem()` splits a `;` list, `toBool()` reads a
 * boolean.
 *
 * @par Thread Safety
 * A value over the caller's text, which must outlive it.
 */
class IniReader {
        public:
                /** @brief What a line is. */
                enum class Kind : uint8_t {
                        Group,   ///< `[name]`: `name()`.
                        Entry,   ///< `key=value`: `key()`, `value()`.
                        Invalid, ///< Neither, nor a comment or blank: `text()`.
                        End,     ///< No more lines.
                };

                /** @brief One line, as `next()` reports it. */
                struct Item {
                                Kind       kind = Kind::End;
                                StringView name;     ///< A group's name; an entry's group.
                                StringView key;      ///< An entry's key (`Name[de]` whole).
                                StringView value;    ///< An entry's value, unescaped not.
                                StringView text;     ///< The line, trimmed.
                                uint32_t   line = 0; ///< From 1.
                };

                /** @brief Reads @p text. */
                explicit constexpr IniReader(StringView text) : _rest(text) {}

                /** @brief Returns the next group, entry or invalid line, or `End`. */
                Item next();

                /** @brief Returns the group the last entry was in ("" before any). */
                StringView group() const { return _group; }

                /**
                 * @brief Takes the next item of the `;` list @p rest (a `\;` doesn't
                 *        split), trimmed, and leaves the remainder in @p rest.
                 *        Empty items are skipped.
                 * @return The item, or an empty view when the list is done.
                 */
                static StringView nextListItem(StringView &rest);

                /**
                 * @brief Writes @p value with the Desktop Entry escapes (`\s`, `\n`,
                 *        `\t`, `\r`, `\\`, `\;`) applied into @p out (NUL-ended).
                 * @return `Ok`, or `BufferTooSmall` (nothing usable written).
                 */
                static Error unescape(StringView value, char *out, size_t outBytes);

                /**
                 * @brief Reads a boolean: `true` or `false` (also `1`, `0`, `yes`,
                 *        `no`, any case).
                 * @return `Ok`, or `Invalid`.
                 */
                static Error toBool(StringView value, bool &out);

        private:
                StringView _rest;
                StringView _group;
                uint32_t   _line = 0;
};

BMDS_NAMESPACE_END
