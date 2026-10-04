/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <cstdio>
#include <cstdlib>
#include <bmds/assert.h>
#include <bmds/vfs.h>

// Host definition of the platform assert hook (the kernel provides the target one).
[[noreturn]] void bmds::Detail::assertFailed(const char *expr, const char *file, int line) {
        std::fprintf(stderr, "BMDS_ASSERT(%s) failed at %s:%d\n", expr, file, line);
        std::abort();
}

namespace {

        // The VFS is set up at boot on the target; here, before any test.
        [[maybe_unused]] const bool vfsReady = bmds::Vfs::init().isOk();

} // namespace
