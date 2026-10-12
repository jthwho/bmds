/**
 * @file      socketfastpath.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A fast-path socket's flow (devplan/network.md §30f): the socket, which is
 * portable, sees the driver's NetFlow through this. The firmware's is
 * socketfastpath.cpp; the host tests have none (open() answers
 * NotSupported).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/socket.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A socket's `NetFlow`, behind an interface the portable socket code can hold.
 *
 * @par Thread Safety
 * `receive()` from the socket's reader, `send()` from its writer (each one at a time); the rest
 * from any task. Never with the core lock held, but `open()` and `close()`, which take it.
 */
class SocketFastPath {
        public:
                /** @brief What the flow is opened for. */
                struct Config {
                                uint32_t address = 0;    ///< The bound address (network order; 0: any).
                                uint16_t port = 0;       ///< The bound port.
                                uint8_t  netifIndex = 0; ///< `SO_BINDTODEVICE`'s interface (lwIP's index; 0: none).
                                uint32_t packetUs = 1000;
                                uint8_t  depth = 4;
                                uint8_t  ttl = 64;
                                uint8_t  multicastTtl = 1;
                                uint8_t  tos = 0;
                                void (*listener)(void *arg) = nullptr; ///< Told of packets and stamps.
                                void *listenerArg = nullptr;
                };

                /** @brief A datagram received. */
                struct Packet {
                                size_t        bytes = 0;
                                size_t        length = 0;
                                SocketAddress from;
                                int64_t       hardwareNs = 0;
                                int64_t       softwareNs = 0;
                };

                /** @brief What `send()` did. */
                struct Sent {
                                uint32_t ticket = 0;      ///< The driver's, for the stamp.
                                size_t   headerBytes = 0; ///< Copied to the caller's `headers`.
                };

                /**
                 * @brief Opens a flow for @p config (memory from `Internal`).
                 * @return It, or `NotSupported` (no fast path here), `NotExist` (no Ethernet
                 *         interface), `Full`, `AddressInUse`, `Invalid`, `NoMem`.
                 */
                static Result<SocketFastPath *> open(const Config &config);

                /** @brief Closes the flow and frees it. */
                virtual void close() = 0;

                /**
                 * @brief Takes (or with @p peek, looks at) the next datagram into @p pieces.
                 * @return It, or `WouldBlock`.
                 */
                virtual Result<Packet> receive(Span<const WritableByteSpan> pieces, bool peek) = 0;

                /** @brief Returns true if a datagram waits. */
                virtual bool readable() = 0;

                /**
                 * @brief Sends to @p peer (network order) from now on, and receives only from it;
                 *        0 address and port: anyone, and no destination. Never waits for ARP.
                 */
                virtual void connect(const SocketAddress &peer) = 0;

                /**
                 * @brief Sends @p pieces to @p to (null: the connected peer), copying the headers
                 *        written to @p headers (`NetClassifier::UdpHeadersBytes`).
                 * @return What it did; `NotSupported` when the flow can't (the caller sends through
                 *         lwIP): larger than a frame, no MAC yet; or `Timeout`, `NotReady`.
                 */
                virtual Result<Sent> send(Span<const ByteSpan> pieces, const SocketAddress *to, bool stamp,
                                          uint8_t *headers, unsigned int timeoutMs) = 0;

                /** @brief Takes the oldest send stamp: @p ticket and @p hardwareNs. */
                virtual bool takeStamp(uint32_t &ticket, int64_t &hardwareNs) = 0;

                /** @brief Returns true if a send stamp waits. */
                virtual bool hasStamp() = 0;

                /** @brief Sets the IP header's fields from now on. */
                virtual void setIpFields(uint8_t ttl, uint8_t multicastTtl, uint8_t tos) = 0;

        protected:
                ~SocketFastPath() = default;
};

BMDS_NAMESPACE_END
