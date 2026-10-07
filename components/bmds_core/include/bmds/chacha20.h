/**
 * @file      chacha20.h
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
 * @brief The ChaCha20 stream cipher's key stream (RFC 8439): what
 *        `/dev/urandom` hands out, keyed from the hardware generator.
 *
 * @par Thread Safety
 * A value; one per user.
 */
class ChaCha20 {
        public:
                /** @brief Bytes of key. */
                static constexpr size_t KeyBytes = 32;

                /** @brief Bytes of nonce. */
                static constexpr size_t NonceBytes = 12;

                /** @brief Bytes of one block. */
                static constexpr size_t BlockBytes = 64;

                /** @brief Keys the stream; it starts at block @p counter. */
                ChaCha20(const uint8_t (&key)[KeyBytes], const uint8_t (&nonce)[NonceBytes], uint32_t counter = 0);

                /** @brief Wipes the key. */
                ~ChaCha20();

                ChaCha20(const ChaCha20 &) = delete;
                ChaCha20 &operator=(const ChaCha20 &) = delete;

                /** @brief Writes the next block of key stream to @p out. */
                void block(uint8_t (&out)[BlockBytes]);

                /** @brief Fills @p size bytes at @p out with key stream (whole blocks; a partial last one's rest is dropped). */
                void fill(uint8_t *out, size_t size);

        private:
                uint32_t _state[16];
};

BMDS_NAMESPACE_END
