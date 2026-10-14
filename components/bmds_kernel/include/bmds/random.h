/**
 * @file      random.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The kernel's random numbers (devplan/random.md): a pool seeded at
 *        boot from the hardware's noise source, fed from the hardware
 *        generator on every read and from `/cfg/random-seed`, read through
 *        ChaCha20. What `/dev/urandom`, `/dev/random` and `getrandom` give.
 *
 * @par Thread Safety
 * Thread-safe in tasks (a mutex, held for microseconds). Not ISR-safe.
 */
namespace Random {

        /**
         * @brief Boot stage: seeds the pool from the hardware's main noise
         *        source (turned on for the moment: on the P4 the SAR ADC).
         *        Before anything reads.
         */
        Error init();

        /** @brief Mixes @p size bytes at @p data into the pool (a seed file). */
        void mix(const void *data, size_t size);

        /** @brief Fills @p size bytes at @p out with random bytes. Never blocks for entropy. */
        void fill(void *out, size_t size);

} // namespace Random

BMDS_NAMESPACE_END
