/**
 * @file      fakefonts.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font system calls on the host (fakefonts.cpp), for the fake system call table.
 */

#pragma once

#include <bmds/sys.h>

namespace FakeFonts {

        /** @brief Puts the font calls into @p slots (a table of `BMDS_SYS_FONT_CLOSE + 1` or more). */
        void install(bmds_sys_fn_t *slots);

} // namespace FakeFonts
