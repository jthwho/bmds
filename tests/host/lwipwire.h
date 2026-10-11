/**
 * @file      lwipwire.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * lwIP for the host tests (devplan/network.md §15, §18): the firmware's
 * lwipopts.h on two Ethernet interfaces wired back to back (a: 10.0.0.1,
 * b: 10.0.0.2), lwIP's port, and NetStack's core lock. Driven by hand
 * (pump(), idle(): lwIP's time is made up) or by a network thread
 * (NetThread: real time) for tests that block, as sockets do.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include "lwipport.h"

namespace LwipWire {

        struct Call {
                        tcpip_callback_fn fn;
                        void             *ctx;
        };

        /** @brief lwIP's scheduled calls (loopback's poll), run by pump() or the thread. */
        extern std::vector<Call> calls;

        /** @brief One side of the wire: its interface, the frames sent to it, the filter requests. */
        struct Port {
                        struct netif                     netif = {};
                        uint8_t                          mac[6] = {};
                        Port                            *peer = nullptr;
                        std::deque<std::vector<uint8_t>> inbox;
                        std::vector<std::string>         filterLog;
        };

        extern Port a;
        extern Port b;

        /** @brief lwIP and both interfaces, once. */
        void setUp();

        /** @brief @p text as an address. */
        ip4_addr_t addr(const char *text);

        /** @brief Delivers what is on the wire and runs the calls and timers, a few rounds of 10 ms. */
        void pump();

        /** @brief Lets @p ms of lwIP's time pass, then pumps. */
        void idle(u32_t ms);

        /** @brief lwIP's heap in use now. */
        size_t heapUsed();

        /**
         * @brief The network task while it lives: frames, calls and timers in
         *        real time under the core lock, which lwIP's calls then check.
         *        Waits for lwIP's next timeout or NetStack::wake(). No pump()
         *        or idle() meanwhile.
         */
        class NetThread {
                public:
                        NetThread();
                        ~NetThread();
                        NetThread(const NetThread &) = delete;
                        NetThread &operator=(const NetThread &) = delete;
        };

} // namespace LwipWire
