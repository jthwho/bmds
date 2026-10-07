/**
 * @file      mnemonic.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/inputevent.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Painter;

/**
 * @brief A text's key, marked with `&` (`devplan/gui.md` §4r): "&OK" shows
 *        "OK", its O underlined, and Alt+O (or O alone, where the keys don't
 *        go to a line edit) chooses it. `&&` is a `&`; a `&` at the end is
 *        dropped. Pure (host-tested), but for `draw()`.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Mnemonic {

        /** @brief No character is marked. */
        constexpr size_t None = SIZE_MAX;

        /** @brief A text with its marks taken out. */
        struct Stripped {
                        size_t   length = 0; ///< The bytes written, without the NUL.
                        size_t   at = None;  ///< The marked character's byte offset in them, or `None`.
                        uint32_t key = 0;    ///< Its key (`keyOf()`'s form), or 0.
        };

        /**
         * @brief Writes @p text without its marks into @p out, NUL-ended (cut on a
         *        character's start to fit; at least a byte). The first marked
         *        character is the one.
         */
        Stripped strip(StringView text, Span<char> out);

        /** @brief Returns @p codepoint as a key: an ASCII letter lowercased, else itself. */
        constexpr uint32_t fold(uint32_t codepoint) {
                return codepoint >= 'A' && codepoint <= 'Z' ? codepoint + ('a' - 'A') : codepoint;
        }

        /**
         * @brief Returns the key @p event chooses, folded: its character, or with
         *        Alt (when a keymap gives none) the letter or digit of its key; 0
         *        for none, a release, or with Ctrl or the Windows key down.
         */
        uint32_t keyOf(const KeyEvent &event);

        /**
         * @brief Underlines character @p at (a byte offset, or `None`: nothing) of
         *        @p text drawn at (@p x, @p y) in unscii-16, on the font's last row,
         *        in @p color, no further right than @p right.
         */
        void draw(Painter &painter, uint16_t x, uint16_t y, StringView text, size_t at, uint32_t color, uint32_t right);

} // namespace Mnemonic

BMDS_NAMESPACE_END
