/**
 * @file      simdsetup.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

namespace Simd {

        /**
         * @brief Turns on PIE's misaligned loads and stores (CFG bits 1 and 0) on both cores, for
         *        good: a boot stage.
         *
         * CFG is the core's, not the task's (FreeRTOS's lazy PIE switch doesn't save it), so the
         * kernels never change it: a kernel that set it and put the old value back, preempted by
         * another doing the same, could leave the other running with misaligned access off
         * (`devplan/simd.md` §5). Nothing else may write CFG.
         *
         * @return `Error::Ok`, or the cross-core call's failure.
         * @par Thread Safety
         * Once, at boot.
         */
        Error setUp();

} // namespace Simd

BMDS_NAMESPACE_END
