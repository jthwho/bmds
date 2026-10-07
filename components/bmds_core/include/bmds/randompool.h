/**
 * @file      randompool.h
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
 * @brief A random pool: one 32-byte key that input is hashed into (BLAKE2s)
 *        and that keys a ChaCha20 stream for each read, replaced as it is
 *        used ("fast key erasure"; devplan/random.md).
 *
 * The arithmetic only: the kernel's `Random` adds the lock and the
 * hardware's entropy.
 *
 * @par Thread Safety
 * A value; the caller locks.
 */
class RandomPool {
        public:
                /** @brief Bytes of key. */
                static constexpr size_t KeyBytes = 32;

                /** @brief An unseeded pool (all zeros: mix something in first). */
                constexpr RandomPool() = default;

                /** @brief Mixes @p size bytes at @p data into the key. */
                void mix(const void *data, size_t size);

                /**
                 * @brief Gives the key for one read in @p readKey and replaces the
                 *        pool's own, so neither can be worked out from the other.
                 */
                void next(uint8_t (&readKey)[KeyBytes]);

                /** @brief Fills @p size bytes at @p out from @p readKey (`next()`'s). */
                static void generate(const uint8_t (&readKey)[KeyBytes], void *out, size_t size);

                /** @brief Overwrites @p size bytes at @p data with zeros, kept by the compiler. */
                static void wipe(void *data, size_t size);

        private:
                uint8_t _key[KeyBytes] = {};
};

BMDS_NAMESPACE_END
