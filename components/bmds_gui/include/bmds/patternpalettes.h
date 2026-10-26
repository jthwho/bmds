/**
 * @file      patternpalettes.h
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
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The desktop patterns' palettes (`devplan/wallpaper.md` §4c): the ones a pattern
 *        carries (PNG `tEXt` chunks keyed `Palette:NAME`), and the user's saved ones, an INI
 *        text with a group for each pattern (by name) whose `Current` is the colors it last had
 *        and whose other entries are saved palettes. Colors as `panel/desktop/pattern-colors`
 *        has them. Pure: the text is read and written by the caller (`PatternFiles`).
 *
 * @par Thread Safety
 * Reentrant; results are views into the caller's text, which must outlive them.
 */
namespace PatternPalettes {

        /** @brief A palette's name at most, in bytes. */
        inline constexpr size_t MaxName = 32;

        /** @brief The key of the colors a pattern last had. */
        inline constexpr const char *CurrentKey = "Current";

        /** @brief The name shown for a pattern's own `PLTE`. */
        inline constexpr const char *OriginalName = "Original";

        /** @brief A palette: its name and its colors (views). */
        struct Palette {
                        StringView name;
                        StringView colors;
        };

        /**
         * @brief Reads @p png's built-in palettes (its `Palette:NAME` chunks) into @p out, as many
         *        as fit, in the file's order.
         * @return How many it has.
         */
        size_t builtIn(ByteSpan png, Span<Palette> out);

        /**
         * @brief Reads @p pattern's saved palettes in @p text into @p out, as many as fit, in the
         *        text's order (`Current` not among them).
         * @return How many it has.
         */
        size_t saved(StringView text, StringView pattern, Span<Palette> out);

        /**
         * @brief Reads @p pattern's `Current` in @p text into @p colors.
         * @return True if it has one.
         */
        bool current(StringView text, StringView pattern, StringView &colors);

        /**
         * @brief Writes @p text into @p out with @p pattern's @p key set to @p colors: its entry
         *        replaced, else added at the end of its group, else in a new group at the end.
         *        Other lines are kept as they are.
         * @return `Ok`, `Invalid` (a name the text can't hold), or `NoMem`.
         */
        Error set(StringView text, StringView pattern, StringView key, StringView colors, String &out);

        /**
         * @brief Writes @p text into @p out without @p pattern's @p key.
         * @return `Ok`, `NotExist` (no such entry: @p out is @p text), or `NoMem`.
         */
        Error remove(StringView text, StringView pattern, StringView key, String &out);

        /**
         * @brief Returns true if @p name can be a saved palette's: 1 to `MaxName` bytes, no
         *        spaces at its ends, none of `=`, `[`, `]`, `;`, `#` or a control character, and not
         *        `Current` or `Original`.
         */
        bool isValidName(StringView name);

} // namespace PatternPalettes

BMDS_NAMESPACE_END
