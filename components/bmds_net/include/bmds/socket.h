/**
 * @file      socket.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's sockets (devplan/network.md §7, §18): TCP, UDP and raw
 * ICMP over lwIP, each a Stream behind a FileHandle.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/deadline.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/** @brief An IPv4 address and port (IPv6 joins it later). */
struct SocketAddress {
                uint32_t ipv4 = 0; ///< The address in network order (0: any).
                uint16_t port = 0; ///< The port in host order (0: any).
};

/**
 * @brief A socket: lwIP's raw API driven from the caller's task under the
 *        core lock (`NetStack`), with lwIP's callbacks (network task) waking
 *        the callers.
 *
 * Received data waits in lwIP's heap: a TCP socket keeps the segments and
 * opens its window as they are read; a UDP or raw socket queues datagrams
 * (`MaxDatagrams`, up to `Option::ReceiveBuffer` bytes) and counts those it
 * drops. Sending copies into lwIP: TCP's send buffer (waiting while it is
 * full), or a buffer per datagram.
 *
 * A call that can wait takes `DontWait` in its flags (`WouldBlock`, or
 * `InProgress` for a connect); otherwise it waits for the socket's timeout
 * (`ReceiveTimeoutMs`, `SendTimeoutMs`: `WouldBlock` when it passes) and is
 * ended by the caller's interrupt (`Interrupted`). As a Stream, `read()`
 * and `write()` are `receive()` and `send()` without flags.
 *
 * The socket is freed when its last handle closes: a TCP connection with
 * unread data (or `Linger` 0) is reset, otherwise closed, lwIP finishing
 * the close on its own.
 *
 * @par Thread Safety
 * Thread-safe. Readers are served one at a time, as are writers.
 */
class Socket final : public Stream {
        public:
                /** @brief The kinds of socket. */
                enum class Type : uint8_t {
                        Stream,   ///< TCP.
                        Datagram, ///< UDP.
                        Raw,      ///< Raw IP of one protocol (ICMP): received with the IP header.
                };

                /** @brief Flags of `send()`, `receive()`, `accept()` and `connect()`. */
                enum Flag : uint32_t {
                        DontWait = 1u << 0, ///< Never wait: `WouldBlock` (`InProgress` for a connect).
                        Peek = 1u << 1,     ///< Receive without taking.
                        WaitAll = 1u << 2,  ///< TCP: wait until the buffers are full (or the end).
                        More = 1u << 3,     ///< TCP: more follows; don't push yet.
                };

                /** @brief Options set and read as integers (`setOption()`, `option()`). */
                enum class Option : uint8_t {
                        ReuseAddress,     ///< Bind to a port in TIME-WAIT (bool).
                        KeepAlive,        ///< TCP keep-alive probes (bool).
                        KeepIdle,         ///< Seconds idle before the first probe.
                        KeepInterval,     ///< Seconds between probes.
                        KeepCount,        ///< Probes unanswered before the connection is dropped.
                        NoDelay,          ///< TCP: no Nagle (bool).
                        ReceiveBuffer,    ///< Bytes of datagrams kept (TCP: its window, fixed).
                        SendBuffer,       ///< Bytes of TCP's send buffer (fixed).
                        ReceiveTimeoutMs, ///< How long a receive waits (0: for ever).
                        SendTimeoutMs,    ///< How long a send or connect waits (0: for ever).
                        Broadcast,        ///< May send to broadcast addresses (bool; recorded).
                        Linger,           ///< -1: off; 0: a close resets; more: recorded (a close never waits).
                        Ttl,              ///< IP's time to live.
                        Tos,              ///< IP's type of service (DSCP and ECN).
                        MulticastTtl,     ///< Time to live of multicast sent.
                        MulticastLoop,    ///< Multicast sent is received here too (bool; on by default).
                        Listening,        ///< Read only: listening for connections (bool).
                        MaxSegment,       ///< Read only: TCP's segment size (bytes).
                };

                /** @brief Datagrams a UDP or raw socket keeps. */
                static constexpr size_t MaxDatagrams = 16;

                /** @brief Connections waiting for `accept()`, at most. */
                static constexpr size_t MaxBacklog = 8;

                /** @brief Multicast groups one socket may join. */
                static constexpr size_t MaxGroups = 8;

                /** @brief Bytes of datagrams kept by default (`ReceiveBuffer`). */
                static constexpr int32_t DefaultReceiveBuffer = 16 * 1024;

                /**
                 * @brief Makes an IPv4 socket of @p type for @p protocol (0: the
                 *        type's own; `Raw` needs one, 1 for ICMP).
                 * @return Its handle (read and write), or `NotSupported` (the
                 *         type and protocol), `NoMem`.
                 */
                static Result<FileHandle::Ptr> create(Type type, uint8_t protocol);

                /** @brief Returns the type. */
                Type type() const { return _type; }

                /** @brief Returns the IP protocol (6 TCP, 17 UDP, 1 ICMP). */
                uint8_t protocol() const { return _protocol; }

                /**
                 * @brief Gives the socket its local address and port (0: any, an
                 *        ephemeral port).
                 * @return `Ok`, `AddressInUse`, `AddressNotAvailable` (not one of
                 *         this system's), `Invalid` (bound already), `NoMem`.
                 */
                Error bind(const SocketAddress &address);

                /**
                 * @brief Listens for connections (TCP), at most @p backlog waiting
                 *        (clamped to 1-`MaxBacklog`); binds an ephemeral port first
                 *        if unbound.
                 * @return `Ok`, `NotSupported` (not TCP), `Invalid` (connected), `NoMem`.
                 */
                Error listen(int backlog);

                /**
                 * @brief Takes a connection that came in.
                 * @param peer Set to its address; may be null.
                 * @param flags `DontWait`.
                 * @return Its handle, or `WouldBlock`, `Invalid` (not listening),
                 *         `Interrupted`, `NoMem`.
                 * @blocking
                 */
                Result<FileHandle::Ptr> accept(SocketAddress *peer, uint32_t flags);

                /**
                 * @brief Connects (TCP), or sets the address datagrams go to and
                 *        come from (UDP, raw; port and address 0 forget it).
                 * @param flags `DontWait`: TCP answers `InProgress` and goes on;
                 *        writable readiness and `takeError()` tell the outcome.
                 * @return `Ok`, `InProgress`, `AlreadyInProgress`, `AlreadyConnected`,
                 *         `ConnectionRefused`, `Timeout`, `NetworkUnreachable`,
                 *         `Interrupted` (it goes on), `WouldBlock` (the timeout).
                 * @blocking
                 */
                Error connect(const SocketAddress &address, uint32_t flags);

                /**
                 * @brief Sends @p pieces as one stream of bytes (TCP) or one datagram.
                 * @param to The datagram's destination; null for the connected one.
                 * @param flags `DontWait`, `More`.
                 * @return Bytes sent (TCP: fewer only with `DontWait` or after an
                 *         interrupt or timeout, which then aren't errors), or
                 *         `WouldBlock`, `NotConnected`, `BrokenPipe`, `ConnectionReset`,
                 *         `MessageTooLong`, `NetworkUnreachable`, `Interrupted`, `NoMem`.
                 * @blocking
                 */
                Result<size_t> send(Span<const ByteSpan> pieces, const SocketAddress *to, uint32_t flags);

                /** @brief What `receive()` got besides the bytes. */
                struct Received {
                                size_t        bytes = 0;  ///< Bytes copied.
                                size_t        length = 0; ///< A datagram's whole length (more than `bytes`: cut).
                                SocketAddress from;       ///< A datagram's source; TCP: the peer.
                };

                /**
                 * @brief Receives into @p pieces: what TCP has (at least a byte), or
                 *        one datagram (cut to fit; the rest is dropped).
                 * @param flags `DontWait`, `Peek`, `WaitAll`.
                 * @return What was received, `EndOfFile` (TCP's peer finished, or
                 *         the receiving side was shut), `WouldBlock`, `NotConnected`,
                 *         `ConnectionReset`, `Interrupted`.
                 * @blocking
                 */
                Result<Received> receive(Span<const WritableByteSpan> pieces, uint32_t flags);

                /**
                 * @brief Shuts the receiving side (reads end), the sending side (TCP
                 *        sends its FIN), or both.
                 * @return `Ok`, `NotConnected`.
                 */
                Error shutdown(bool receiving, bool sending);

                /** @brief Returns the local address and port. */
                SocketAddress address();

                /** @brief Returns the peer's address and port, or `NotConnected`. */
                Result<SocketAddress> peer();

                /** @brief Sets @p option to @p value. @return `Ok`, `NotSupported`, `Invalid`. */
                Error setOption(Option option, int32_t value);

                /** @brief Returns @p option's value, or `NotSupported`. */
                Result<int32_t> option(Option option);

                /** @brief Returns and clears the pending error (`Ok` if none). */
                Error takeError();

                /**
                 * @brief Joins multicast @p group (network order) on the interface
                 *        with address @p interface (0: every one).
                 * @return `Ok`, `NotSupported` (TCP), `Invalid` (not multicast),
                 *         `Exists`, `Full` (`MaxGroups`), `AddressNotAvailable`, `NoMem`.
                 */
                Error joinGroup(uint32_t group, uint32_t interface);

                /** @brief Leaves a group joined. @return `Ok`, `NotExist`. */
                Error leaveGroup(uint32_t group, uint32_t interface);

                /** @brief Sends multicast from the interface with address @p interface (0: the route's). */
                Error setMulticastInterface(uint32_t interface);

                /** @brief Returns the interface address multicast is sent from (0: the route's). */
                uint32_t multicastInterface();

                /**
                 * @brief Sends and receives only through interface @p name ("eth0"; empty: any).
                 * @return `Ok`, `NotExist`.
                 */
                Error bindToDevice(StringView name);

                /** @brief Datagrams dropped because the queue was full. */
                uint32_t dropped() const { return _dropped; }

                // Stream
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                uint8_t   readiness() override;
                WaitList *readinessWaiters() override { return &_pollers; }
                Socket   *socket() override { return this; }
                void      handleClosed(uint8_t mode) override;

        private:
                friend struct SocketCallbacks;

                struct Datagram {
                                void    *pbuf = nullptr; // lwIP's pbuf
                                uint32_t ipv4 = 0;
                                uint16_t port = 0;
                };

                struct Group {
                                uint32_t group = 0;
                                uint32_t interface = 0;
                };

                Socket(Type type, uint8_t protocol);
                ~Socket() = default;

                static Socket *make(Type type, uint8_t protocol);
                void           destroy();

                Result<size_t>   sendWithin(Span<const ByteSpan> pieces, const SocketAddress *to, uint32_t flags,
                                            unsigned int timeoutMs);
                Result<Received> receiveWithin(Span<const WritableByteSpan> pieces, uint32_t flags,
                                               unsigned int timeoutMs);
                unsigned int     timeoutOf(const std::atomic<int32_t> &optionMs, unsigned int timeoutMs) const;

                // Core lock held.
                Error            makePcb();
                void             attachTcp();
                void             closePcb();
                void             dropReceived();
                void             queue(void *pbuf, uint32_t from, uint16_t port);
                void             inheritOptions(const Socket &listener);
                void             applyOptions();
                void             wakeAll();
                bool             isReadable() const;
                bool             isWritable() const;
                Result<size_t>   sendTcp(Span<const ByteSpan> pieces, uint32_t flags, const Deadline &deadline);
                Result<size_t>   sendDatagram(Span<const ByteSpan> pieces, const SocketAddress *to);
                Result<Received> receiveLocked(Span<const WritableByteSpan> pieces, uint32_t flags, size_t skip);

                const Type    _type;
                const uint8_t _protocol;

                // Under the core lock.
                void    *_pcb = nullptr;      // lwIP's tcp_pcb, udp_pcb or raw_pcb; null once freed
                void    *_received = nullptr; // TCP: the pbuf chain not yet read
                size_t   _receivedBytes = 0;
                Datagram _datagrams[MaxDatagrams];
                size_t   _datagramHead = 0;
                size_t   _datagramCount = 0;
                size_t   _datagramBytes = 0;
                Socket  *_pending[MaxBacklog] = {}; // accepted by lwIP, not yet by accept()
                size_t   _pendingCount = 0;
                Group    _groups[MaxGroups];
                size_t   _groupCount = 0;
                Error    _error; // pending: takeError(), or the next call
                bool     _listening = false;
                bool     _connecting = false;
                bool     _connected = false;
                bool     _gone = false;         // TCP: lwIP freed the PCB (reset, aborted, refused)
                bool     _peerFinished = false; // TCP: the FIN came; reads end
                bool     _receiveShut = false;
                bool     _sendShut = false;
                uint32_t _dropped = 0;

                // The options (Linux's defaults), applied to the PCB and inherited by accepted ones.
                bool     _reuseAddress = false;
                bool     _keepAlive = false;
                bool     _noDelay = false;
                bool     _broadcast = false;
                bool     _multicastLoop = true;
                uint8_t  _ttl = 64;
                uint8_t  _tos = 0;
                uint8_t  _multicastTtl = 1;
                int32_t  _keepIdle = 7200;
                int32_t  _keepInterval = 75;
                int32_t  _keepCount = 9;
                int32_t  _linger = -1;
                int32_t  _receiveBuffer = DefaultReceiveBuffer;
                uint32_t _multicastInterface = 0;

                std::atomic<int32_t> _receiveTimeoutMs{0};
                std::atomic<int32_t> _sendTimeoutMs{0};
                Mutex                _readLock;
                Mutex                _writeLock;
                Semaphore            _readable{1, 0};
                Semaphore            _writable{1, 0};
                WaitListOf<4>        _pollers;
};

BMDS_NAMESPACE_END
