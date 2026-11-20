/**
 * @file      iochipops.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The chip's pad registers (devplan/io.md §3.1): each chip implements these in
 * chips/<target>/iochipops.cpp, and nothing else in BMDS touches a pad's
 * registers for the I/O view. Target only.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/iopad.h>

BMDS_NAMESPACE_BEGIN

namespace IoChipOps {

        /**
         * @brief Reads pad @p pad's configuration and level.
         * @nonblocking
         */
        void readPad(unsigned pad, IoPadState &out);

        /**
         * @brief Where input signal @p signal comes from through the GPIO matrix:
         *        a pad, `IoChip::constZeroInput` or `constOneInput`; or -1 when the
         *        matrix doesn't route it (it comes through IO_MUX, or isn't used).
         * @nonblocking
         */
        int inputSource(unsigned signal);

} // namespace IoChipOps

BMDS_NAMESPACE_END
