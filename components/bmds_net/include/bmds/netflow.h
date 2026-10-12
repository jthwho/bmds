/**
 * @file      netflow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Real-time flows (devplan/network.md §5, §30d): frames a flow matches go
 * from the Ethernet driver's task straight to it, and its frames go straight
 * to the driver, never through lwIP, its heap or the network task.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ethernet.h>
#include <bmds/ethernetdevice.h>
#include <bmds/netclassifier.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/spinlock.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class NetInterface;

/**
 * @brief A real-time flow on an Ethernet device: what it receives (frames of
 *        an EtherType, or UDP datagrams to a port), held in a ring of its own,
 *        and what it sends, with the departure stamps handed to it.
 *
 * Opening a flow holds bulk traffic (TCP, everything lwIP sends and receives)
 * to its share of the driver, so a flow within its depth always receives and
 * sends, however much else the link carries: 2 transmit descriptors (1 for a
 * packet time under 1 ms) and 8 receive buffers kept for flows, while any is
 * open. A flow's sender should run on core 1, away from the driver's task
 * (devplan/network.md §30c).
 *
 * The owner keeps the object (nothing is allocated): `open()` it, `close()` it
 * before it goes.
 *
 * @par Thread Safety
 * `open()`, `close()`, `connect()` and `setDestination()` from one task at a
 * time, not with the network stack's core lock held. The rest from any tasks
 * while the flow is open.
 */
class NetFlow final : public TxStampSink {
        public:
                /** @brief Flows a device takes at once. */
                static constexpr size_t MaxFlows = EthernetDevice::MaxFlows;

                /** @brief The deepest a flow may be: all open flows' depths together are at most this. */
                static constexpr size_t MaxDepth = EthernetDevice::RealTimeRxReserve;

                /** @brief Departure stamps a flow keeps until taken. */
                static constexpr size_t StampDepth = 8;

                /** @brief Send flags. */
                enum SendFlag : uint32_t {
                        Stamp = 1u << 0, ///< Hand the departure time to `receiveTxStamp()`.
                };

                /** @brief What the flow receives, and how it sends. */
                struct Config {
                                NetClassifier::Kind kind = NetClassifier::Kind::Udp;
                                uint16_t            etherType = 0; ///< `EtherType`: the type (PTP's 0x88f7).
                                MacAddress          group; ///< `EtherType`: a group to receive too (zero: none).
                                uint32_t            address =
                                        0;         ///< `Udp`: the address to receive on: a group (joined) or 0 for any.
                                uint16_t port = 0; ///< `Udp`: the port to receive on, and to send from.
                                uint32_t packetUs = 1000; ///< The shortest time between its packets, either way.
                                uint8_t  depth = 4;       ///< Frames it may hold, queued and taken (1 to `MaxDepth`).
                                uint8_t  ttl = 64;        ///< `Udp`: sent datagrams' time to live.
                                uint8_t  dscp = 0;        ///< `Udp`: sent datagrams' DSCP (AES67's media: 34).
                };

                /** @brief A received packet: its frame (the driver's buffer, until `release()`). */
                struct Packet {
                                EthernetDevice::RxFrame frame;
                                uint16_t                offset = 0;     ///< The payload's start in the frame.
                                uint16_t                length = 0;     ///< The payload's bytes.
                                uint32_t                sourceIp = 0;   ///< `Udp`: the sender (host order).
                                uint16_t                sourcePort = 0; ///< `Udp`: the sender's port.
                                MacAddress              source;         ///< The frame's source.
                                int64_t softwareNs = 0; ///< The system clock (UTC) when the driver gave it to the flow.

                                /** @brief Returns the payload. */
                                Span<const uint8_t> payload() const {
                                        return Span<const uint8_t>(frame.data + offset, length);
                                }
                };

                /** @brief A sent frame's departure time. */
                struct TxStamp {
                                uint32_t ticket = 0; ///< As `send()` returned it.
                                PtpTime  time;
                };

                /** @brief Counters since `open()`. */
                struct Counters {
                                uint64_t rxPackets = 0;
                                uint32_t rxDropped = 0; ///< Frames past the depth (the receiver fell behind).
                                uint64_t txPackets = 0;
                                uint32_t txErrors = 0;     ///< Sends the driver refused (link down, no room in time).
                                uint32_t txStamps = 0;     ///< Departure stamps handed to the flow.
                                uint32_t txStampsLost = 0; ///< Stamps dropped from a full queue (the oldest).
                };

                /** @brief Told when a packet or a stamp comes (after the flow's waiters). @nonblocking */
                using Listener = void (*)(void *arg);

                constexpr NetFlow() = default;
                ~NetFlow() { close(); }
                NetFlow(const NetFlow &) = delete;
                NetFlow &operator=(const NetFlow &) = delete;

                /**
                 * @brief Registers the flow on @p device: matching frames come to it from now on,
                 *        and bulk traffic is held to its share. A `Udp` flow on a group joins it.
                 * @return `Ok`; `Busy` if open; `Invalid` (a kind, port or depth out of range, a
                 *         group that isn't one); `Full` (`MaxFlows`, or the depths past `MaxDepth`);
                 *         `AddressInUse` (another flow matches the same); `NotReady` (a `Udp` flow
                 *         on a device without an IP interface); `NoMem` (joining the group).
                 * @blocking Briefly.
                 */
                Error open(EthernetDevice &device, const Config &config);

                /**
                 * @brief Takes the flow out: frames queued are released (those taken must be
                 *        released by their takers), a group left. Nothing if not open.
                 * @blocking Briefly.
                 */
                void close();

                /**
                 * @brief Sets who else is told of packets and stamps (a socket's readers and
                 *        pollers): called in the driver's or a sender's task, never under a lock.
                 *        Only while closed.
                 */
                void setListener(Listener listener, void *arg) {
                        _listener = listener;
                        _listenerArg = arg;
                        return;
                }

                /** @brief Returns true while open. */
                bool isOpen() const { return _open.load(std::memory_order_acquire); }

                /** @brief Returns the configuration (while open). */
                const Config &config() const { return _config; }

                /** @brief Returns the device (while open). */
                EthernetDevice *device() const { return _device; }

                /**
                 * @brief Waits for a packet.
                 * @return The packet (`release()` it); `Timeout`; `NotReady` if not open.
                 * @blocking
                 */
                Result<Packet> receive(unsigned int timeoutMs);

                /** @brief As `receive()`, without waiting: `WouldBlock`. @nonblocking */
                Result<Packet> tryReceive();

                /** @brief Gives a packet's buffer back. Any task. @nonblocking */
                void release(const Packet &packet);

                /** @brief Returns the packets queued. @nonblocking */
                size_t queued() const;

                /**
                 * @brief Sets where a `Udp` flow sends: @p address and @p port (host order). A group's
                 *        MAC is its own; a unicast address's (or its gateway's, off the subnet) comes
                 *        from ARP, waiting up to @p timeoutMs for an answer, and is kept current
                 *        without lwIP.
                 * @return `Ok`; `NotReady` (not open, no address on the interface);
                 *         `NetworkUnreachable` (off the subnet with no gateway); `HostUnreachable`
                 *         (no ARP answer in time); `Invalid`.
                 * @blocking
                 */
                Error connect(uint32_t address, uint16_t port, unsigned int timeoutMs);

                /**
                 * @brief As `connect()`, without waiting: `InProgress` when ARP must answer first
                 *        (asked now; the snooped answer finishes it, and `send()` answers
                 *        `NotConnected` until then).
                 * @nonblocking
                 */
                Error tryConnect(uint32_t address, uint16_t port);

                /** @brief Forgets the destination (`send()` answers `NotConnected`). */
                void disconnect();

                /**
                 * @brief Receives only what @p address and @p port send (`Udp`; 0 for any): a
                 *        connected socket's narrowing. @return `Ok`, `NotReady`, `Invalid`.
                 * @blocking Briefly.
                 */
                Error filterSource(uint32_t address, uint16_t port);

                /** @brief Sets where an `EtherType` flow sends. @return `Ok`, `Invalid`, `NotReady`. */
                Error setDestination(const MacAddress &destination);

                /**
                 * @brief Sends @p payload to the destination, with the flow's headers (a `Udp`
                 *        flow's Ethernet, IPv4 and UDP; an `EtherType` flow's Ethernet), ahead of
                 *        bulk traffic.
                 * @param flags     `SendFlag` bits.
                 * @param timeoutMs How long to wait for a descriptor, or `WaitForever`.
                 * @return A ticket (the `TxStamp`'s); `NotConnected` (no destination); `NotReady`
                 *         (not open, no address, link down); `MessageTooLong` (no fragments);
                 *         `Timeout`.
                 * @blocking
                 */
                Result<uint32_t> send(Span<const uint8_t> payload, uint32_t flags, unsigned int timeoutMs);

                /** @brief Where one `Udp` datagram goes (`send()`). */
                struct Destination {
                                uint32_t   address = 0; ///< Host order.
                                uint16_t   port = 0;
                                MacAddress mac;
                };

                /**
                 * @brief As `send()`, the payload gathered from @p pieces, to @p to when not null (a
                 *        `Udp` flow: address and port in host order, and the MAC to send to) or else
                 *        the destination; the headers written are copied to @p headers when not null
                 *        (`NetClassifier::UdpHeadersBytes` there).
                 * @blocking
                 */
                Result<uint32_t> send(Span<const Span<const uint8_t>> pieces, const Destination *to, uint32_t flags,
                                      unsigned int timeoutMs, uint8_t *headers);

                /**
                 * @brief Sets a `Udp` flow's IPv4 time to live (@p multicastTtl to groups) and DSCP
                 *        from now on. @nonblocking
                 */
                void setIpFields(uint8_t ttl, uint8_t multicastTtl, uint8_t dscp);

                /** @brief Returns the bytes of headers this flow's frames have. */
                size_t headerBytes() const;

                /**
                 * @brief Returns the MAC that sending to @p address (host order) goes to now: a
                 *        group's, broadcast's, or the next hop's from lwIP's ARP table; zero if
                 *        ARP hasn't one (nothing is asked).
                 * @blocking Briefly (the core lock).
                 */
                MacAddress macFor(uint32_t address);

                /**
                 * @brief Waits for a departure stamp of a frame sent with `Stamp`, oldest first.
                 * @return The stamp; `Timeout`; `NotReady` if not open.
                 * @blocking
                 */
                Result<TxStamp> receiveTxStamp(unsigned int timeoutMs);

                /** @brief As `receiveTxStamp()`, without waiting: `WouldBlock`. @nonblocking */
                Result<TxStamp> tryReceiveTxStamp();

                /** @brief Returns the stamps waiting. @nonblocking */
                size_t txStampsQueued() const;

                /** @brief Returns the counters. @nonblocking */
                Counters counters() const;

                /** @brief Returns who waits for packets and stamps (to join a `poll()`). */
                WaitList &waiters() { return _waiters; }

                /** @brief An open flow as `list()` shows it. */
                struct Info {
                                Config     config;
                                Counters   counters;
                                size_t     queued = 0;
                                uint32_t   destinationIp = 0; ///< `Udp`, connected: where it sends.
                                uint16_t   destinationPort = 0;
                                MacAddress destinationMac;
                };

                /** @brief Fills @p out with @p device's open flows (`net`); returns how many. @blocking Briefly. */
                static size_t list(EthernetDevice &device, Span<Info> out);

        private:
                friend class EthernetDevice;

                // The driver's task: a frame matched. False when the flow is full (the driver drops it).
                bool deliver(const EthernetDevice::RxFrame &frame, const NetClassifier::Match &match);
                // The driver: a frame's departure stamp (after its transmit lock).
                void stamped(uint32_t cookie, uint32_t ticket, const PtpTime &time) override;
                void tell();
                void route(uint32_t address, uint32_t &local, uint32_t &hop, MacAddress &mac, bool &unreachable);
                // The driver's task, its flow lock held: an ARP frame's sender. True if connect() waits
                // for it (the driver wakes the waiters after the lock).
                bool learn(uint32_t ip, const MacAddress &mac);
                void sendArpRequest(uint32_t sourceIp);

                EthernetDevice   *_device = nullptr; // kept after close(): taken packets are released to it
                Listener          _listener = nullptr;
                void             *_listenerArg = nullptr;
                NetClassifier::Rule _rule; // as registered (filterSource() narrows it)
                std::atomic<bool> _open{false};
                Config            _config;
                NetInterface     *_interface = nullptr; // a Udp flow's

                mutable SpinLock _lock; // the rings, the destination, the counters
                Packet           _ring[MaxDepth];
                size_t           _head = 0;
                size_t           _count = 0;
                size_t           _held = 0; // queued and taken
                TxStamp          _stamps[StampDepth];
                size_t           _stampHead = 0;
                size_t           _stampCount = 0;
                Counters         _counters;
                WaitListOf<4>    _waiters;

                // Sending: the destination and its next hop (whose ARP answers keep its MAC).
                NetClassifier::UdpEndpoints _endpoints;   // the sources filled in at each send
                uint32_t                    _nextHop = 0; // 0: none to ask (a group, broadcast)
                bool                        _connected = false;
                uint16_t                    _id = 0;
                uint8_t                     _multicastTtl = 64; // under _lock
                uint64_t                    _arpAskedUs = 0; // under _lock
};

BMDS_NAMESPACE_END
