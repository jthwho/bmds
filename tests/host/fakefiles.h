/**
 * @file      fakefiles.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The file system calls on the host (fakefiles.cpp), for the fake system call table: `open` (to
 * read only), `read`, `fstat` and `close` on the host's own files, for the SDK's libraries'
 * loaders (libbmds3d's models).
 */

#pragma once

#include <bmds/sys.h>

namespace FakeFiles {

        /** @brief Puts `open`, `read`, `fstat` and `close` into @p slots. */
        void install(bmds_sys_fn_t *slots);

} // namespace FakeFiles
