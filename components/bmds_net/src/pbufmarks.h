/**
 * @file      pbufmarks.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What rides in a pbuf besides its bytes (lwipopts.h's LWIP_PBUF_CUSTOM_DATA,
 * devplan/network.md §30f): a received frame's arrival times, and a datagram's
 * transmit class and stamp cookie.
 */

#pragma once

#include <cstdint>
#include <lwip/pbuf.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace PbufMarks {

        /** @brief `bmds_tx_class`: how the driver sends the frame. */
        enum TxClass : uint8_t {
                Bulk = 0,     ///< Within bulk's share.
                Priority = 1, ///< `EthernetDevice::TxPriority`.
        };

        inline void setNs(u32_t (&to)[2], int64_t ns) {
                to[0] = static_cast<u32_t>(static_cast<uint64_t>(ns));
                to[1] = static_cast<u32_t>(static_cast<uint64_t>(ns) >> 32);
                return;
        }

        inline int64_t ns(const u32_t (&from)[2]) {
                return static_cast<int64_t>(static_cast<uint64_t>(from[0]) | static_cast<uint64_t>(from[1]) << 32);
        }

        /** @brief Sets @p p's arrival times (0: none). */
        inline void setArrival(struct pbuf *p, int64_t hardwareNs, int64_t softwareNs) {
                setNs(p->bmds_rx_hw, hardwareNs);
                setNs(p->bmds_rx_sw, softwareNs);
                return;
        }

        /** @brief Copies @p from's arrival times to @p to (a copy lwIP doesn't carry them to). */
        inline void copyArrival(struct pbuf *to, const struct pbuf *from) {
                to->bmds_rx_hw[0] = from->bmds_rx_hw[0];
                to->bmds_rx_hw[1] = from->bmds_rx_hw[1];
                to->bmds_rx_sw[0] = from->bmds_rx_sw[0];
                to->bmds_rx_sw[1] = from->bmds_rx_sw[1];
                return;
        }

        /** @brief Returns @p p's hardware arrival time (ns, the 1588 clock; 0: none). */
        inline int64_t hardwareNs(const struct pbuf *p) {
                return ns(p->bmds_rx_hw);
        }

        /** @brief Returns @p p's system arrival time (ns UTC; 0: none). */
        inline int64_t softwareNs(const struct pbuf *p) {
                return ns(p->bmds_rx_sw);
        }

} // namespace PbufMarks

BMDS_NAMESPACE_END
