/**
 * @file      ed25519.h
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
 * @brief Ed25519 (RFC 8032), Monocypher's (thirdparty/monocypher): checking update packages'
 *        signatures (devplan/update.md §2). Verifying takes about 10 ms on the board.
 *
 * @par Thread Safety
 * Pure functions.
 */
namespace Ed25519 {

        /** @brief Bytes of a public key, a seed, and a signature. */
        constexpr size_t PublicKeyBytes = 32;
        constexpr size_t SeedBytes = 32;      ///< See `PublicKeyBytes`.
        constexpr size_t SignatureBytes = 64; ///< See `PublicKeyBytes`.

        /** @brief Returns true if @p signature is @p key's over the @p size bytes at @p message. */
        bool verify(const uint8_t (&signature)[SignatureBytes], const uint8_t (&key)[PublicKeyBytes],
                    const void *message, size_t size);

        /**
         * @brief Signs the @p size bytes at @p message with the key made from @p seed (its public half
         *        to @p publicKey). For tests and tools; the board only verifies.
         */
        void sign(const uint8_t (&seed)[SeedBytes], const void *message, size_t size,
                  uint8_t (&signature)[SignatureBytes], uint8_t (&publicKey)[PublicKeyBytes]);

} // namespace Ed25519

BMDS_NAMESPACE_END
