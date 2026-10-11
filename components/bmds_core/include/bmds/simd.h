/**
 * @file      simd.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

// The ESP32-P4's PIE vector unit and hardware loops, when this build targets them.
#if defined(ESP_PLATFORM) && defined(__riscv_xespv) && defined(__riscv_xesploop)
#define BMDS_HAS_PIE 1
#else
#define BMDS_HAS_PIE 0
#endif

BMDS_NAMESPACE_BEGIN

/**
 * @brief Which way a vector kernel runs (`devplan/simd.md`): each has a plain
 *        C++ twin that the PIE one matches byte for byte, so tests can run
 *        both (`simd test`).
 *
 * @par Thread Safety
 * Constants.
 */
namespace Simd {

        /** @brief A kernel's way. */
        enum class Kernel : uint8_t {
                Scalar, ///< Plain C++ (the host's; the reference).
                Pie,    ///< The ESP32-P4's PIE unit.
        };

        /** @brief Returns true if this build has the PIE kernels. */
        constexpr bool hasPie() {
                return BMDS_HAS_PIE != 0;
        }

        /** @brief Returns the fastest way this build has. */
        constexpr Kernel best() {
                return hasPie() ? Kernel::Pie : Kernel::Scalar;
        }

        /** @brief Returns @p k, or `Scalar` if this build lacks it. */
        constexpr Kernel usable(Kernel k) {
                return k == Kernel::Pie && !hasPie() ? Kernel::Scalar : k;
        }

} // namespace Simd

BMDS_NAMESPACE_END
