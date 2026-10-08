/**
 * @file      audiocontrol.h
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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One setting a device's hardware offers: the ES8311's DAC slot, its
 *        volume (`devplan/audio.md` §16).
 *
 * A control is plain data (rodata): its value is an `int32_t` whose meaning
 * the type gives. A `Decibel` value is in hundredths of a decibel; an `Enum`
 * value is the index of a word of `choices`; a `Bool` is 0 or 1. Names and
 * values are part of the `audio` command's interface once shipped.
 *
 * @par Thread Safety
 * Immutable; thread-safe.
 */
struct AudioControl {
                /** @brief How the value reads. */
                enum class Type : uint8_t {
                        Bool,    ///< `on` / `off` (also `true`, `false`, `yes`, `no`, `1`, `0`).
                        Enum,    ///< One of the words of `choices`.
                        Decibel, ///< Hundredths of a dB, written as `-12.5` (`dB` optional).
                        Int,     ///< A whole number.
                };

                /** @brief The longest text `format()` writes, with its NUL. */
                static constexpr size_t MaxText = 24;

                const char *name = "";        ///< Its name: lowercase, digits and `-`.
                const char *help = "";        ///< One line on what it does.
                Type        type = Type::Int; ///< How the value reads.
                int32_t     min = 0;          ///< Smallest value (`Enum`: 0).
                int32_t     max = 0;          ///< Largest value (`Enum`: the last word's index).
                int32_t     step = 1;         ///< Values are `min` plus a multiple of this.
                const char *choices = "";     ///< `Enum`: its words, separated by `|`.

                /**
                 * @brief Reads @p text as a value of this control.
                 * @return The value, `Invalid` (not of the type, or between steps),
                 *         or `OutOfRange`.
                 * @nonblocking
                 */
                Result<int32_t> parse(StringView text) const;

                /**
                 * @brief Writes @p value as text into @p out (at least `MaxText` bytes).
                 * @nonblocking
                 */
                void format(int32_t value, char *out, size_t size) const;

                /** @brief Returns the type's name: `bool`, `enum`, `dB`, `int`. */
                const char *typeName() const;

                /** @brief Returns word @p index of `choices` (empty if none). */
                StringView choice(int32_t index) const;
};

BMDS_NAMESPACE_END
