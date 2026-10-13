/**
 * @file      sha1.h
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
 * @brief SHA-1 (FIPS 180-4), for checking what was published with one: IANA's leap-second list
 *        (devplan/network.md §31e). Not for anything that must resist an attacker.
 *
 * @par Thread Safety
 * A plain object: not thread-safe.
 */
class Sha1 {
        public:
                /** @brief Bytes of digest. */
                static constexpr size_t DigestBytes = 20;

                /** @brief Starts a hash. */
                Sha1();

                /** @brief Hashes @p size more bytes at @p data. */
                void update(const void *data, size_t size);

                /** @brief Writes the digest to @p out; the hash is spent. */
                void final(uint8_t (&out)[DigestBytes]);

        private:
                static constexpr size_t BlockBytes = 64;

                void compress();

                uint32_t _h[5];
                uint64_t _bytes = 0; // hashed
                uint8_t  _block[BlockBytes] = {};
                size_t   _used = 0; // bytes in _block
};

BMDS_NAMESPACE_END
