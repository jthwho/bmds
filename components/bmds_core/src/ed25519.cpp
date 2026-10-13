/**
 * @file      ed25519.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/ed25519.h>

extern "C" {
#include <monocypher-ed25519.h>
#include <monocypher.h>
}

BMDS_NAMESPACE_BEGIN

bool Ed25519::verify(const uint8_t (&signature)[SignatureBytes], const uint8_t (&key)[PublicKeyBytes],
                     const void *message, size_t size) {
        return crypto_ed25519_check(signature, key, static_cast<const uint8_t *>(message), size) == 0;
}

void Ed25519::sign(const uint8_t (&seed)[SeedBytes], const void *message, size_t size,
                   uint8_t (&signature)[SignatureBytes], uint8_t (&publicKey)[PublicKeyBytes]) {
        uint8_t copy[SeedBytes];
        std::memcpy(copy, seed, sizeof(copy)); // key_pair wipes the seed it is given
        uint8_t secret[64];
        crypto_ed25519_key_pair(secret, publicKey, copy);
        crypto_ed25519_sign(signature, secret, static_cast<const uint8_t *>(message), size);
        crypto_wipe(secret, sizeof(secret));
        return;
}

BMDS_NAMESPACE_END
