/**
 * @file      rxcopies.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <lwip/pbuf.h>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Received frames copied into PSRAM (devplan/network.md §32): what TCP's window holds once
 *        lwIP keeps more ring buffers than `NetInterface::MaxHeld`.
 *
 * A pool of frame-sized slots made at start, handed out from a free list: a copy is a custom pbuf
 * over its slot, and its free gives the slot back. No heap call and no cache work (only the CPU
 * touches the slots).
 *
 * @par Thread Safety
 * `init()` before the stack runs; the rest under the core lock, as every pbuf's free is (lwIP's
 * heap has no other lock).
 */
namespace RxCopies {

        /** @brief The largest frame a slot holds (`EthernetDevice::BufferBytes`). */
        constexpr size_t SlotBytes = 1536;

        /** @brief How the pool is used. */
        struct Usage {
                        size_t   slots = 0; ///< In the pool.
                        size_t   used = 0;  ///< Held now.
                        size_t   most = 0;  ///< The most held at once.
                        uint32_t empty = 0; ///< Copies wanted with none free.
        };

        /**
         * @brief Makes the pool: as many slots as fit in @p bytes of PSRAM (0: none, every copy
         *        refused). Again: the old pool freed first, which must have none held.
         * @return `Ok`, `NoMem`, `Busy` (slots held).
         */
        Error init(size_t bytes);

        /**
         * @brief Copies the frame at @p data, @p length bytes, into a slot.
         * @return Its pbuf (`PBUF_RAW`), or null: the pool empty, or a frame too big.
         * @hotpath
         */
        struct pbuf *copy(const uint8_t *data, size_t length);

        /** @brief Whether @p p is one of the pool's copies (a socket may keep it: no ring buffer held). */
        bool isCopy(const struct pbuf *p);

        /** @brief Returns how the pool is used. */
        Usage usage();

} // namespace RxCopies

BMDS_NAMESPACE_END
