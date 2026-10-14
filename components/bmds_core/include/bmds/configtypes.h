/**
 * @file      configtypes.h
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
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system configuration's parameters: keys, types, their text
 *        forms, and the store's line format (`devplan/sysconf.md` §2, §4).
 *
 * Pure: the registry (`SysConf`) keeps the values; this checks them. Every
 * type has one canonical text form, which `get` returns, the store holds,
 * and the code using a value parses with the `to...()` functions.
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace Config {

        /** @brief A parameter's type. Its name (`typeName()`) is an interface once shipped. */
        enum class Type : uint8_t {
                Bool,   ///< `true` or `false` (also `yes`/`no`, `on`/`off`, `1`/`0` on input).
                Int,    ///< Decimal, within `Param::min` and `Param::max`.
                String, ///< UTF-8, at most `Param::max` bytes (`MaxValue` if 0).
                Color,  ///< `rrggbb` in hex (also `#RRGGBB` on input).
                Enum,   ///< One of `Param::choices` (words separated by `|`).
                Path,   ///< An absolute path, normalized.
                Paths,  ///< Absolute paths separated by `;`, each normalized.
                /**
                 * A string never shown (`system/password`'s hash): stored as `String`
                 * is, but readers get `SecretSet` or "" instead of the value
                 * (`SysConf::get()`, `info()`, `list()`, the SDK); only
                 * `SysConf::getSecret()` gives it. Its declaration's check decides
                 * what may go in.
                 */
                Secret,
        };

        /** @brief What a set `Secret` reads as. */
        inline constexpr StringView SecretSet("(set)");

        /**
         * @brief A further check of a value already in its canonical form
         *        (`home`: an existing directory). Called without the registry's
         *        lock; must not change the registry.
         * @return `Ok`, or the error refusing it.
         */
        using Check = Error (*)(StringView value);

        /** @brief A parameter's declaration: rodata for the built-in ones (`BMDS_SYSCONF`). */
        struct Param {
                        const char *key = nullptr;       ///< `owner/name/...` (`isValidKey()`).
                        Type        type = Type::String; ///< Its type.
                        const char *defaultValue = "";   ///< Its default, in canonical form.
                        const char *help = "";           ///< One line describing it.
                        int32_t     min = 0;             ///< `Int`: the smallest value.
                        int32_t     max = 0; ///< `Int`: the largest; `String`, `Path(s)`: the longest (0: `MaxValue`).
                        const char *choices = nullptr; ///< `Enum`: the words, separated by `|`.
                        Check       check = nullptr;   ///< A further check, or null.
        };

        namespace detail {
                constexpr char hexDigit(uint32_t v) {
                        return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
                }
        } // namespace detail

        /**
         * @brief @p Rgb (0xRRGGBB) as a canonical `Color` value in rodata: a
         *        declaration's default made from a `constexpr` color.
         */
        template <uint32_t Rgb>
        inline constexpr char ColorText[7] = {detail::hexDigit((Rgb >> 20) & 0xf),
                                              detail::hexDigit((Rgb >> 16) & 0xf),
                                              detail::hexDigit((Rgb >> 12) & 0xf),
                                              detail::hexDigit((Rgb >> 8) & 0xf),
                                              detail::hexDigit((Rgb >> 4) & 0xf),
                                              detail::hexDigit(Rgb & 0xf),
                                              '\0'};

        /** @brief The longest key, in bytes. */
        inline constexpr size_t MaxKey = 63;

        /** @brief The longest value, in bytes. */
        inline constexpr size_t MaxValue = 1024;

        /**
         * @brief True if @p key is a valid key: components of lowercase letters,
         *        digits and `-`, separated by single `/`, at most `MaxKey` bytes.
         */
        bool isValidKey(StringView key);

        /**
         * @brief Returns the type's name: "bool", "int", "string", "color", "enum", "path", "paths",
         *        "secret".
         */
        const char *typeName(Type type);

        /**
         * @brief Puts @p input in @p param's canonical form into @p out, checking
         *        it against the type and the limits (not `Param::check`).
         * @return `Ok`, `Invalid` (not of the type), `OutOfRange` (a limit), `NoMem`.
         */
        Error canonical(const Param &param, StringView input, String &out);

        /** @brief A canonical `Int` value's number (0 if it isn't one). */
        int32_t toInt(StringView value);

        /** @brief A canonical `Bool` value's truth. */
        bool toBool(StringView value);

        /** @brief A canonical `Color` value as 0xRRGGBB (0 if it isn't one). */
        uint32_t toColor(StringView value);

        // --- The store (`/cfg/sysconf`): `key=value` lines ---------------------------

        /** @brief Called for each `key=value` line `parseStore()` finds (value unescaped). */
        using StoreLine = void (*)(void *arg, StringView key, StringView value);

        /**
         * @brief Reads @p text as the store: blank lines and `#` comments are
         *        skipped, a line without a valid key and `=` is skipped too.
         * @return The number of lines skipped as malformed.
         */
        size_t parseStore(StringView text, StoreLine fn, void *arg);

        /** @brief Appends `key=value\n` to @p out, the value's `\` and newlines escaped. */
        Error appendStoreLine(String &out, StringView key, StringView value);

} // namespace Config

BMDS_NAMESPACE_END
