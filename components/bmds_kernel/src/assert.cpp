/**
 * @file      assert.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <assert.h>
#include <bmds/assert.h>

// Target definition of the platform assert hook declared in bmds_core.
// Routes through the C library's assert handler, which ESP-IDF turns into a
// panic with the expression, location, and a backtrace.
[[noreturn]] void bmds::Detail::assertFailed(const char *expr, const char *file, int line) {
        __assert_func(file, line, "", expr);
}
