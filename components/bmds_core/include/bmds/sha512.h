/**
 * @file      sha512.h
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
 * @brief The SHA-512 hash (FIPS 180-4): what password hashes are made of
 *        (`ShaCrypt`, devplan/network.md §24). In software; the SHA engine
 *        comes under it in step 5a.
 *
 * @par Thread Safety
 * A value; one per user.
 */
class Sha512 {
        public:
                /** @brief Bytes of digest. */
                static constexpr size_t DigestBytes = 64;

                /** @brief Starts a hash. */
                Sha512();

                /** @brief Hashes @p size more bytes at @p data. */
                void update(const void *data, size_t size);

                /** @brief Writes the digest to @p out; the hash is spent (and wiped). */
                void final(uint8_t (&out)[DigestBytes]);

        private:
                static constexpr size_t BlockBytes = 128;

                void compress();

                uint64_t _h[8];
                uint64_t _bytes = 0; // hashed (2^64 bytes is beyond any use here)
                uint8_t  _block[BlockBytes] = {};
                size_t   _used = 0; // bytes in _block
};

BMDS_NAMESPACE_END
