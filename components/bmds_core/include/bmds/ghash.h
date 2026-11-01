/**
 * @file      ghash.h
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
 * @brief GCM's GHASH (NIST SP 800-38D §6.4) with 8-bit tables (Shoup's): what the firmware's
 *        AES-GCM uses in place of ESP-IDF's 4-bit one (devplan/network.md §33). The chip has
 *        no carry-less multiply.
 *
 * One table of H times each byte (4 KB) made from the hash key; a block is 16 steps of a shift,
 * a reduction from a 512-byte constant table and a lookup. Table lookups depend on the data, as
 * every table GHASH's do (ESP-IDF's and mbedTLS's): not constant-time against a local observer of
 * the caches.
 *
 * @par Thread Safety
 * A value; one per user.
 */
class Ghash {
        public:
                /** @brief Bytes in a block (and in the key and the value). */
                static constexpr size_t BlockBytes = 16;

                /** @brief Makes the tables for hash key @p h (the cipher's encryption of zeros). */
                void setKey(const uint8_t (&h)[BlockBytes]);

                /**
                 * @brief Hashes @p size bytes at @p data into @p y, a block at a time, the last
                 *        padded with zeros: y = (y ^ block) * H.
                 * @hotpath
                 */
                void update(uint8_t (&y)[BlockBytes], const void *data, size_t size) const;

        private:
                // H times each byte value, the block's bytes as 4 big-endian words.
                uint32_t _table[256][4];
};

BMDS_NAMESPACE_END
