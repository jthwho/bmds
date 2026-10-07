/**
 * @file      blake2s.h
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
 * @brief The BLAKE2s hash (RFC 7693), 32-byte digests, unkeyed: what the
 *        kernel's random pool mixes its input with (devplan/random.md).
 *
 * @par Thread Safety
 * A value; one per user.
 */
class Blake2s {
        public:
                /** @brief Bytes of digest. */
                static constexpr size_t DigestBytes = 32;

                /** @brief Starts a hash. */
                Blake2s();

                /** @brief Hashes @p size more bytes at @p data. */
                void update(const void *data, size_t size);

                /** @brief Writes the digest to @p out; the hash is spent (and wiped). */
                void final(uint8_t (&out)[DigestBytes]);

        private:
                static constexpr size_t BlockBytes = 64;

                void compress(bool last);

                uint32_t _h[8];
                uint32_t _t[2] = {}; // bytes hashed
                uint8_t  _block[BlockBytes] = {};
                size_t   _used = 0; // bytes in _block
};

BMDS_NAMESPACE_END
