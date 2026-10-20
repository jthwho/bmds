/**
 * @file      netservices.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Services the board advertises by multicast DNS (DNS-SD,
 *        `devplan/network.md` §22): the SSH server's `_ssh._tcp`, network MIDI's
 *        `_apple-midi._udp` (`devplan/audio.md` §23), on every interface mDNS
 *        answers on, while `net/mdns/enabled` is on.
 *
 * @par Thread Safety
 * Any task, without the core lock.
 */
namespace NetServices {

        /** @brief Services at once (lwIP's `MDNS_MAX_SERVICES`). */
        constexpr size_t MaxServices = 2;

        /** @brief The longest service type (`_apple-midi`). */
        constexpr size_t MaxType = 23;

        /** @brief The longest instance name (a DNS label). */
        constexpr size_t MaxName = 63;

        /**
         * @brief Advertises @p service (`_apple-midi`) over UDP or TCP on @p port,
         *        named @p instance (empty: the host's name, following it); port 0
         *        takes it back.
         * @return `Ok`, `Full` (`MaxServices` others), `Invalid` (a type or name too long),
         *         or `InvalidState` (no network stack).
         */
        Error advertise(StringView service, bool udp, uint16_t port, StringView instance);

} // namespace NetServices

BMDS_NAMESPACE_END
