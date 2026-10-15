/**
 * @file      chord.h
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
 * @brief A key and the modifiers held with it, written as text
 *        (`devplan/hotkeys.md` §3.1): `Ctrl+Shift+T`, `F5`, `Left`, `Ctrl`.
 *
 * Keys are HID extended usages ((page << 16) | usage). Modifiers are
 * written side-less (`Ctrl` is either Ctrl key), as are the modifier keys
 * themselves when bound alone (`Ctrl`); `LeftCtrl` and the like name one
 * side. A key without a name is written as its extended usage in hex,
 * `0x000C00E9`. Names are read in any case and written in the canonical one.
 *
 * @par Thread Safety
 * A plain value.
 */
struct Chord {
                /** @brief Side-less modifier bits. */
                enum Modifier : uint8_t {
                        Ctrl = 0x1,
                        Shift = 0x2,
                        Alt = 0x4,
                        Win = 0x8,
                };

                /** @brief Bytes `format()` may write, NUL included. */
                static constexpr size_t MaxText = 40;

                uint32_t usage = 0;          ///< The key; 0: none.
                uint8_t  modifiers = 0;      ///< `Modifier` bits that must be down with it.
                bool     eitherSide = false; ///< A modifier key named side-less: the right one too.

                /** @brief Returns true if it names a key. */
                constexpr bool isValid() const { return usage != 0; }

                /**
                 * @brief Reads @p text (surrounding spaces ignored).
                 * @return `Invalid` if a part isn't a modifier or a key.
                 */
                static Error parse(StringView text, Chord &out);

                /** @brief Writes it as text into @p out. @return The length (truncated to fit). */
                size_t format(char *out, size_t size) const;

                /** @brief Returns the side-less modifiers of a boot report's modifier byte. */
                static uint8_t modifiersOf(uint8_t bootModifiers);

                /**
                 * @brief Returns true if a key event of @p usage with @p modifiers (side-less) down is
                 *        this chord: the key itself (either side, for a side-less modifier key), and
                 *        at least its modifiers down (a modifier key's own bit set aside).
                 * @hotpath
                 */
                bool matches(uint32_t usage, uint8_t modifiers) const;

                /** @brief Bytes of a key's name, NUL included. */
                static constexpr size_t MaxName = 16;

                /** @brief Writes @p usage's name into @p out. @return False for a key without one. */
                static bool nameOf(uint32_t usage, char (&out)[MaxName]);
};

BMDS_NAMESPACE_END
