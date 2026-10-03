/**
 * @file      memspace.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A kind of memory that allocations come from.
 *
 * Every allocation in BMDS names a MemSpace. What each space maps to
 * (heap capabilities, fallbacks when a board lacks PSRAM) is defined by the
 * board definition (`bmds_board`); allocation itself is in `bmds_kernel`.
 *
 * Kernel-internal: not part of the userspace ABI.
 */
enum class MemSpace : uint8_t {
        Internal,    ///< Internal SRAM. Fast, scarce: stacks, kernel objects, hot data.
        InternalDma, ///< Internal SRAM usable by DMA: descriptors, small DMA buffers.
        Psram,       ///< External PSRAM. Large, slower, cached: bulk data, program images.
        PsramDma,    ///< PSRAM usable by DMA, cache-line aligned: framebuffers, large DMA buffers.
        Fast,        ///< Tightly-coupled memory (TCM). Tiny, single-cycle: the hottest data only.
};

/** @brief Number of MemSpace values. */
inline constexpr size_t MemSpaceCount = 5;

/**
 * @brief Returns the identifier of a MemSpace, e.g. `"Psram"`.
 * @return A static string (never null).
 */
constexpr const char *memSpaceName(MemSpace space) {
        switch (space) {
                case MemSpace::Internal: return "Internal";
                case MemSpace::InternalDma: return "InternalDma";
                case MemSpace::Psram: return "Psram";
                case MemSpace::PsramDma: return "PsramDma";
                case MemSpace::Fast: return "Fast";
        }
        return "Unknown";
}

BMDS_NAMESPACE_END
