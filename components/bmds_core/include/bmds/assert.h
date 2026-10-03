/**
 * @file      assert.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace Detail {

        /**
         * @brief Reports a failed `BMDS_ASSERT` and halts.
         *
         * Declared here and defined by the platform: the kernel on the target
         * (panics with a backtrace), the test harness on the host.
         */
        [[noreturn]] void assertFailed(const char *expr, const char *file, int line);

} // namespace Detail

BMDS_NAMESPACE_END

/**
 * @def BMDS_ASSERT
 * @brief Checks a programmer invariant; halts the system if it is violated.
 *
 * For programmer errors only. Runtime conditions (out of memory, I/O
 * errors, bad user input, bad syscall arguments from user programs) must
 * return an `Error` instead. When asserts are compiled out the condition
 * is not evaluated, so it must not have side effects.
 */
#if BMDS_ENABLE_ASSERT
#define BMDS_ASSERT(cond)                                                                                              \
        do {                                                                                                           \
                if (!(cond)) [[unlikely]]                                                                              \
                        ::bmds::Detail::assertFailed(#cond, BMDS_SOURCE_FILE, __LINE__);                               \
        } while (0)
#else
#define BMDS_ASSERT(cond)                                                                                              \
        do {                                                                                                           \
                (void)sizeof(cond);                                                                                    \
        } while (0)
#endif
