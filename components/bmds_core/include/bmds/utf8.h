/**
 * @file      utf8.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief UTF-8 decoding, encoding, and the display width of characters.
 *
 * Malformed input never stalls a decoder: each bad byte becomes
 * `Replacement` (U+FFFD), as terminals show it. Overlong forms, surrogates
 * and values above U+10FFFF are malformed.
 *
 * `displayWidth()` follows wcwidth(): 2 for East Asian wide and fullwidth
 * characters and emoji, 0 for combining marks, zero-width characters and
 * controls, 1 otherwise, from a compact rodata range table (an
 * approximation of Unicode's tables, good for terminal cursor arithmetic).
 *
 * @par Thread Safety
 * Free functions are reentrant; a `Utf8Decoder` belongs to one user.
 */
namespace Utf8 {

        /** @brief U+FFFD REPLACEMENT CHARACTER. */
        inline constexpr uint32_t Replacement = 0xFFFD;

        /** @brief Returns true for a continuation byte (10xxxxxx). */
        constexpr bool isContinuation(uint8_t b) {
                return (b & 0xC0) == 0x80;
        }

        /**
         * @brief Decodes the character at @p text[@p i] and advances @p i past it.
         * @return The code point, or `Replacement` (consuming one byte) if malformed.
         */
        uint32_t decode(StringView text, size_t &i);

        /**
         * @brief Encodes @p codepoint into @p out.
         * @return The byte count (1-4); a value that can't be encoded gives `Replacement`'s 3.
         */
        size_t encode(uint32_t codepoint, char out[4]);

        /** @brief Returns the terminal columns @p codepoint takes: 0, 1 or 2. */
        int displayWidth(uint32_t codepoint);

        /** @brief Returns the terminal columns @p text takes. */
        size_t displayWidth(StringView text);

} // namespace Utf8

/**
 * @brief Decodes UTF-8 a byte at a time (for streams that arrive in pieces).
 */
class Utf8Decoder {
        public:
                /**
                 * @brief Takes one byte.
                 * @param b   The byte.
                 * @param out [out] Code points completed by this byte.
                 * @return How many were completed: 0, 1, or 2 (a malformed sequence
                 *         cut short by a new character gives `Replacement` and then
                 *         that character's result).
                 */
                size_t feed(uint8_t b, uint32_t out[2]);

                /** @brief Returns true in the middle of a sequence. */
                bool isPending() const { return _need != 0; }

                /** @brief Abandons a sequence in progress. */
                void reset() {
                        _need = 0;
                        return;
                }

        private:
                uint32_t _codepoint = 0;
                uint32_t _min = 0;  // smallest value this length may encode (overlong check)
                uint8_t  _need = 0; // continuation bytes still expected
};

BMDS_NAMESPACE_END
