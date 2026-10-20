/**
 * @file      netudp.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A UDP port for a kernel service that lives on the network task
 *        (network MIDI, `devplan/audio.md` §23): no thread and no socket. What
 *        arrives is given to a callback on the network task with the core lock
 *        held; sending is with the core lock held (the callback's, a
 *        `NetStack::callLater()` function's, or a `NetStack::Locker`).
 *
 * Addresses are IPv4 in network order, as `NetStack::resolve()` gives them.
 *
 * @par Thread Safety
 * Every call with the core lock held.
 */
class NetUdp {
        public:
                /**
                 * @brief A datagram from @p address and @p port. @p data is valid during
                 *        the call. Network task, core lock held: don't wait.
                 */
                using Receive = void (*)(void *arg, uint32_t address, uint16_t port, ByteSpan data);

                constexpr NetUdp() = default;
                ~NetUdp() { close(); }
                NetUdp(const NetUdp &) = delete;
                NetUdp &operator=(const NetUdp &) = delete;

                /**
                 * @brief Takes @p port on every interface; @p receive gets what arrives.
                 * @return `Ok`, `InvalidState` (open), `Exists` (the port is taken), or `NoMem`.
                 */
                Error open(uint16_t port, Receive receive, void *arg);

                /** @brief Gives the port back. */
                void close();

                /** @brief Returns true while open. */
                bool isOpen() const { return _pcb != nullptr; }

                /**
                 * @brief Sends @p data to @p address and @p port from this port.
                 * @return `Ok`, `InvalidState` (closed), `NoMem` (lwIP's heap), or
                 *         `NetworkUnreachable` (no route; nothing sent).
                 */
                Error sendTo(uint32_t address, uint16_t port, ByteSpan data);

        private:
                friend struct NetUdpCallbacks;

                void    *_pcb = nullptr; // struct udp_pcb
                Receive  _receive = nullptr;
                void    *_arg = nullptr;
                uint8_t *_scratch = nullptr; // a datagram in pieces, joined (PSRAM, made when needed)
};

BMDS_NAMESPACE_END
