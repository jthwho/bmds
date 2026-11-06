/**
 * @file      fakememory.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The memory system calls on the host (fakememory.cpp), for the fake system call table: what
 * the SDK's libraries allocate with bmds_mem_alloc(), counted for leak checks.
 */

#pragma once

#include <cstddef>
#include <bmds/sys.h>

namespace FakeMemory {

        /** @brief Puts `mem_alloc` and `mem_free` into @p slots. */
        void install(bmds_sys_fn_t *slots);

        /** @brief Allocations not yet freed. */
        size_t live();

} // namespace FakeMemory
