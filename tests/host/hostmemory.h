/**
 * @file      hostmemory.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Host definition of bmds::Memory: malloc with allocation counting (for
 * leak checks) and failure injection (for NoMem paths).
 */

#pragma once

#include <cstddef>

namespace HostMemory {

        /** @brief Number of Memory allocations not yet freed. */
        size_t liveAllocations();

        /**
         * @brief Makes allocations fail after @p successes more succeed.
         * @param successes Allocations still allowed; negative means never fail.
         */
        void failAfter(long successes);

        /** @brief Fails allocations from the next one until the guard is destroyed. */
        struct FailGuard {
                        explicit FailGuard(long successes = 0) { failAfter(successes); }
                        ~FailGuard() { failAfter(-1); }
                        FailGuard(const FailGuard &) = delete;
                        FailGuard &operator=(const FailGuard &) = delete;
        };

} // namespace HostMemory
