/**
 * @file      fontname.h
 * @copyright Jason Howard. All rights reserved.
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
 * @brief A font's name as one string (`devplan/fonts.md` §4a): what settings,
 *        the GUI protocol, angel and the SDK write. A family, a pixel size and
 *        a style (`IBM Plex Sans 13 bold italic`), or a role with what it
 *        overrides (`ui`, `title 20`, `mono bold`).
 *
 * Read from the end: a number is the size (4-255), `bold`, `italic` and
 * `regular` the style (any case); the words before them are the family,
 * spaces between them made single. A family that is one role's word is that
 * role. So a family can't end in a number.
 *
 * @par Thread Safety
 * A value.
 */
struct FontName {
                /** @brief The roles (`font/ui`, ...): the fonts the user chooses once for everything. */
                enum class Role : uint8_t {
                        None,     ///< A family named outright.
                        Ui,       ///< The toolkit's text.
                        Mono,     ///< Fixed-width text outside the terminal.
                        Title,    ///< Window titles and headings.
                        Terminal, ///< The terminal's cells.
                };

                /** @brief Style bits. */
                enum Style : uint8_t {
                        Bold = 0x01,
                        Italic = 0x02,
                };

                /** @brief The number of roles, `None` aside. */
                static constexpr size_t RoleCount = 4;

                /** @brief The longest family, in bytes. */
                static constexpr size_t MaxFamily = 63;

                /** @brief The longest name `format()` writes, its terminator included. */
                static constexpr size_t MaxText = MaxFamily + 24;

                /** @brief The smallest and largest pixel sizes a name may give. */
                static constexpr uint16_t MinSize = 4, MaxSize = 255;

                char     family[MaxFamily + 1] = {}; ///< Empty for a role.
                uint8_t  familyLength = 0;
                Role     role = Role::None;
                uint16_t size = 0;         ///< Pixels per em; 0: not given (the role's, or the default).
                uint8_t  style = 0;        ///< `Style` bits.
                bool     hasStyle = false; ///< A style word was given (`regular` gives one with no bits).

                /**
                 * @brief Reads @p text.
                 * @return The name, `Invalid` (no family or role), or `OutOfRange` (a
                 *         size outside 4-255, a family longer than `MaxFamily`).
                 */
                static Result<FontName> parse(StringView text);

                /** @brief A name for @p familyText at @p pixelSize with @p styleBits. */
                static FontName make(StringView familyText, uint16_t pixelSize, uint8_t styleBits);

                /** @brief Returns the family. */
                StringView familyName() const { return StringView(family, familyLength); }

                /**
                 * @brief Writes the name as `parse()` reads it into @p out (at most
                 *        @p capacity bytes, terminated).
                 * @return The length written.
                 */
                size_t format(char *out, size_t capacity) const;

                /** @brief Returns @p role's word (`ui`, ...; "" for `None`). */
                static const char *roleName(Role role);

                /** @brief Returns the role @p word names, or `None`. */
                static Role roleFromName(StringView word);
};

BMDS_NAMESPACE_END
