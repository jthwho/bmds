/**
 * @file      shacrypt.h
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
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief SHA-512 crypt (Ulrich Drepper's "Unix crypt using SHA-256 and
 *        SHA-512", crypt(3)'s `$6$`): the form `system/password` holds
 *        (devplan/network.md §24), which musl's `crypt()` also reads.
 *
 * A hash is `$6$rounds=N$SALT$HASH`: the salt up to 16 of `./0-9A-Za-z`,
 * the hash 86 of them. Without `rounds=` a setting means 5000 rounds.
 *
 * @par Thread Safety
 * Stateless: callable from any task.
 */
namespace ShaCrypt {

        /** @brief Rounds when a setting doesn't say. */
        inline constexpr uint32_t DefaultRounds = 5000;

        /** @brief The fewest and most rounds; a setting's beyond them are clamped (the algorithm's rule). */
        inline constexpr uint32_t MinRounds = 1000;
        inline constexpr uint32_t MaxRounds = 999'999'999; ///< See `MinRounds`.

        /** @brief The longest salt used (a longer one is cut, as the algorithm says). */
        inline constexpr size_t MaxSalt = 16;

        /** @brief The longest password taken, in bytes. */
        inline constexpr size_t MaxPassword = 256;

        /** @brief Characters of the hash part. */
        inline constexpr size_t HashChars = 86;

        /** @brief A setting or hash, read by `parse()`; the views point into the text. */
        struct Setting {
                        uint32_t   rounds = DefaultRounds; ///< Clamped to `MinRounds`-`MaxRounds`.
                        bool       roundsGiven = false;    ///< `rounds=` was there.
                        StringView salt;                   ///< As written (the algorithm uses 16 at most).
                        StringView hash;                   ///< Empty in a setting without one.
        };

        /**
         * @brief Reads @p text: `$6$`, optionally `rounds=N$`, a salt of `./0-9A-Za-z`,
         *        then optionally `$` and the hash.
         * @return True if it is one (a hash part, if any, isn't checked here).
         */
        bool parse(StringView text, Setting &out);

        /**
         * @brief True if @p text is a complete hash: a salt of at most 16 and an
         *        86-character hash.
         */
        bool isHash(StringView text);

        /**
         * @brief Hashes @p password with @p salt (cut to 16; `./0-9A-Za-z`) and
         *        @p rounds (clamped), into @p out as `$6$rounds=N$SALT$HASH`.
         * @return `Ok`, `Invalid` (a bad salt, a password over `MaxPassword`), `NoMem`.
         */
        Error hash(StringView password, StringView salt, uint32_t rounds, String &out);

        /**
         * @brief True if @p password matches @p hash (a complete one). The
         *        comparison takes the same time whatever differs.
         */
        bool verify(StringView password, StringView hash);

} // namespace ShaCrypt

BMDS_NAMESPACE_END
