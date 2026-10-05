/**
 * @file      memtest.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Memory test patterns (`devplan/psram.md`): each word's value comes
 *        from a key (its address, normally), so every word differs and any
 *        stretch of memory can be filled or checked on its own.
 *
 * @par Thread Safety
 * Pure functions on the caller's memory.
 */
namespace MemTest {

        /** @brief What each word holds. */
        enum class Pattern : uint8_t {
                Address,         ///< The key itself (finds address line faults).
                InvertedAddress, ///< Its inverse (every bit the other way).
                Checkerboard,    ///< 0x55555555 and 0xAAAAAAAA in alternate words.
                Random,          ///< A hash of the key and the seed (data-dependent faults).
        };

        /** @brief The patterns, in the order a pass runs them. */
        inline constexpr Pattern Patterns[] = {Pattern::Address, Pattern::InvertedAddress, Pattern::Checkerboard,
                                               Pattern::Random};

        /** @brief A word that read back wrong. */
        struct Failure {
                        uintptr_t address = 0;  ///< Where.
                        uint32_t  expected = 0; ///< What was written.
                        uint32_t  actual = 0;   ///< What was read.
        };

        /** @brief Returns the pattern's name, e.g. `"address"`. */
        const char *name(Pattern pattern);

        /**
         * @brief Returns the word for key @p key (a word address: a multiple
         *        of 4) in @p pattern, with @p seed (each pass's own).
         */
        uint32_t expected(Pattern pattern, uint32_t seed, uintptr_t key);

        /**
         * @brief Fills @p count words at @p words, keyed from @p key on (4 a
         *        word).
         * @hotpath
         */
        void fill(Pattern pattern, uint32_t seed, uint32_t *words, size_t count, uintptr_t key);

        /**
         * @brief Checks @p count words at @p words against the pattern keyed
         *        from @p key on.
         * @param failures    Where the first wrong words go (may be null).
         * @param maxFailures Room in @p failures.
         * @return The number of wrong words.
         * @hotpath
         */
        size_t verify(Pattern pattern, uint32_t seed, const uint32_t *words, size_t count, uintptr_t key,
                      Failure *failures, size_t maxFailures);

} // namespace MemTest

BMDS_NAMESPACE_END
