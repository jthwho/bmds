/**
 * @file      socket.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's sockets on lwIP's raw API (devplan/network.md §18). Calls
 * work under the core lock in the caller's task and wait outside it;
 * lwIP's callbacks run in the network task (the lock held) and wake them.
 */

#include <new>
#include <lwip/igmp.h>
#include <lwip/ip.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/raw.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/netstack.h>
#include <bmds/socket.h>

#include "lwipport.h"

BMDS_NAMESPACE_BEGIN

// TCP's unread segments are one pbuf chain, whose length is 16 bits: the window bounds it.
static_assert(TCP_WND + TCP_OOSEQ_MAX_PBUFS * TCP_MSS < 0xffff, "TCP's unread data fits a pbuf chain");

namespace {

        constexpr uint8_t ProtocolIcmp = 1;
        constexpr uint8_t ProtocolTcp = 6;
        constexpr uint8_t ProtocolUdp = 17;

        // The largest datagram's payload: 64 KB less the IP header (and UDP's).
        constexpr size_t MaxRawPayload = 0xffff - 20;
        constexpr size_t MaxUdpPayload = MaxRawPayload - 8;

        // Datagram buffer limits (Option::ReceiveBuffer).
        constexpr int32_t MinReceiveBuffer = 1024;
        constexpr int32_t MaxReceiveBuffer = 256 * 1024;

        Error errorOf(err_t err) {
                switch (err) {
                        case ERR_OK: return Error::Ok;
                        case ERR_MEM:
                        case ERR_BUF: return Error::NoMem;
                        case ERR_TIMEOUT: return Error::Timeout;
                        case ERR_RTE: return Error::NetworkUnreachable;
                        case ERR_INPROGRESS: return Error::InProgress;
                        case ERR_WOULDBLOCK: return Error::WouldBlock;
                        case ERR_USE: return Error::AddressInUse;
                        case ERR_ALREADY: return Error::AlreadyInProgress;
                        case ERR_ISCONN: return Error::AlreadyConnected;
                        case ERR_CONN:
                        case ERR_CLSD: return Error::NotConnected;
                        case ERR_IF: return Error::NetworkDown;
                        case ERR_ABRT: return Error::ConnectionAborted;
                        case ERR_RST: return Error::ConnectionReset;
                        default: return Error::Invalid;
                }
        }

        ip_addr_t ipOf(uint32_t ipv4) {
                ip_addr_t a;
                ip_addr_set_ip4_u32(&a, ipv4);
                return a;
        }

        // The interface with address @p ipv4, or null.
        struct netif *netifWith(uint32_t ipv4) {
                struct netif *n;
                NETIF_FOREACH(n) {
                        if (ip4_addr_get_u32(netif_ip4_addr(n)) == ipv4) return n;
                }
                return nullptr;
        }

        // An address a socket of @p datagrams may bind to: any, loopback, one of
        // this system's, or (datagrams) multicast or broadcast.
        bool isBindable(uint32_t ipv4, bool datagrams) {
                const ip4_addr_t a = {ipv4};
                if (ip4_addr_isany_val(a) || ip4_addr_isloopback(&a)) return true;
                if (datagrams && (ip4_addr_ismulticast(&a) || ip4_addr_get_u32(&a) == IPADDR_BROADCAST)) return true;
                if (netifWith(ipv4) != nullptr) return true;
                if (datagrams) {
                        struct netif *n;
                        NETIF_FOREACH(n) {
                                if (ip4_addr_isbroadcast(&a, n)) return true;
                        }
                }
                return false;
        }

        size_t totalOf(Span<const ByteSpan> pieces) {
                size_t total = 0;
                for (const ByteSpan &p : pieces) total += p.size();
                return total;
        }

        size_t totalOf(Span<const WritableByteSpan> pieces) {
                size_t total = 0;
                for (const WritableByteSpan &p : pieces) total += p.size();
                return total;
        }

        // Copies @p count bytes of @p p from @p offset into @p pieces, from @p skip bytes into them.
        void copyOut(const struct pbuf *p, size_t offset, Span<const WritableByteSpan> pieces, size_t skip,
                     size_t count) {
                for (const WritableByteSpan &piece : pieces) {
                        if (count == 0) break;
                        if (skip >= piece.size()) {
                                skip -= piece.size();
                                continue;
                        }
                        const size_t n = piece.size() - skip < count ? piece.size() - skip : count;
                        (void)pbuf_copy_partial(p, piece.data() + skip, static_cast<u16_t>(n),
                                                static_cast<u16_t>(offset));
                        offset += n;
                        count -= n;
                        skip = 0;
                }
                return;
        }

        // A received pbuf a socket keeps: a copy if it is the driver's buffer, so the
        // receive ring gets it back at once. Null without memory (@p p is freed).
        struct pbuf *keepable(struct pbuf *p) {
                if ((p->flags & PBUF_FLAG_IS_CUSTOM) == 0) return p;
                struct pbuf *copy = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
                pbuf_free(p);
                return copy;
        }

} // namespace

// --- lwIP's callbacks: the network task, the core lock held ----------------------------------

struct SocketCallbacks {
                static err_t tcpReceived(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
                        auto &s = *static_cast<Socket *>(arg);
                        (void)err;
                        if (p == nullptr) {
                                s._peerFinished = true;
                                s.wakeAll();
                                return ERR_OK;
                        }
                        if (s._receiveShut) {
                                tcp_recved(pcb, p->tot_len);
                                pbuf_free(p);
                                return ERR_OK;
                        }
                        if (s._received == nullptr) {
                                s._received = p;
                        } else {
                                pbuf_cat(static_cast<struct pbuf *>(s._received), p);
                        }
                        s._receivedBytes += p->tot_len;
                        s._readable.release();
                        s._pollers.notify();
                        return ERR_OK;
                }

                static err_t tcpSent(void *arg, struct tcp_pcb *, u16_t) {
                        auto &s = *static_cast<Socket *>(arg);
                        s._writable.release();
                        s._pollers.notify();
                        return ERR_OK;
                }

                // lwIP has freed the PCB (reset, aborted, timed out).
                static void tcpError(void *arg, err_t err) {
                        auto &s = *static_cast<Socket *>(arg);
                        s._pcb = nullptr;
                        s._gone = true;
                        Error e = errorOf(err);
                        if (s._connecting) {
                                // A SYN answered by a reset, or never answered.
                                e = err == ERR_RST ? Error::ConnectionRefused : err == ERR_ABRT ? Error::Timeout : e;
                        }
                        if (err != ERR_CLSD) s._error = e;
                        s._connecting = false;
                        s.wakeAll();
                        return;
                }

                static err_t tcpConnected(void *arg, struct tcp_pcb *, err_t) {
                        auto &s = *static_cast<Socket *>(arg);
                        s._connecting = false;
                        s._connected = true;
                        s.wakeAll();
                        return ERR_OK;
                }

                static err_t tcpAccepted(void *arg, struct tcp_pcb *pcb, err_t err) {
                        auto &l = *static_cast<Socket *>(arg);
                        if (err != ERR_OK || pcb == nullptr) return ERR_VAL;
                        Socket *c = l._pendingCount < Socket::MaxBacklog
                                            ? Socket::make(Socket::Type::Stream, ProtocolTcp)
                                            : nullptr;
                        if (c == nullptr) {
                                tcp_abort(pcb);
                                return ERR_ABRT;
                        }
                        c->inheritOptions(l);
                        c->_pcb = pcb;
                        c->_connected = true;
                        c->attachTcp();
                        c->applyOptions();
                        tcp_backlog_delayed(pcb);
                        l._pending[l._pendingCount++] = c;
                        l._readable.release();
                        l._pollers.notify();
                        return ERR_OK;
                }

                static void udpReceived(void *arg, struct udp_pcb *, struct pbuf *p, const ip_addr_t *from,
                                        u16_t port) {
                        static_cast<Socket *>(arg)->queue(p, ip_addr_get_ip4_u32(from), port);
                        return;
                }

                // A copy for the socket: lwIP goes on with the packet (ICMP answers echoes).
                static u8_t rawReceived(void *arg, struct raw_pcb *, struct pbuf *p, const ip_addr_t *from) {
                        struct pbuf *copy = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
                        if (copy == nullptr) {
                                ++static_cast<Socket *>(arg)->_dropped;
                                return 0;
                        }
                        static_cast<Socket *>(arg)->queue(copy, ip_addr_get_ip4_u32(from), 0);
                        return 0;
                }
};

// --- Making and freeing ---------------------------------------------------------------------

Socket::Socket(Type type, uint8_t protocol) : _type(type), _protocol(protocol) {}

Socket *Socket::make(Type type, uint8_t protocol) {
        void *block = Memory::alloc(MemSpace::Psram, sizeof(Socket));
        return block != nullptr ? new (block) Socket(type, protocol) : nullptr;
}

void Socket::destroy() {
        this->~Socket();
        Memory::free(this);
        return;
}

Result<FileHandle::Ptr> Socket::create(Type type, uint8_t protocol) {
        switch (type) {
                case Type::Stream:
                        if (protocol != 0 && protocol != ProtocolTcp)
                                return makeError<FileHandle::Ptr>(Error::NotSupported);
                        protocol = ProtocolTcp;
                        break;
                case Type::Datagram:
                        if (protocol != 0 && protocol != ProtocolUdp)
                                return makeError<FileHandle::Ptr>(Error::NotSupported);
                        protocol = ProtocolUdp;
                        break;
                case Type::Raw:
                        if (protocol != ProtocolIcmp) return makeError<FileHandle::Ptr>(Error::NotSupported);
                        break;
        }
        Socket *s = make(type, protocol);
        if (s == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        Error err;
        {
                NetStack::Locker lock;
                err = s->makePcb();
        }
        if (err.isError()) {
                s->destroy();
                return makeError<FileHandle::Ptr>(err);
        }
        auto [handle, herr] = FileHandle::open(*s, FileHandle::ReadWrite);
        if (herr.isError()) {
                {
                        NetStack::Locker lock;
                        s->closePcb();
                }
                s->destroy();
                return makeError<FileHandle::Ptr>(herr);
        }
        return makeResult(std::move(handle));
}

Error Socket::makePcb() {
        switch (_type) {
                case Type::Stream: {
                        struct tcp_pcb *pcb = tcp_new();
                        if (pcb == nullptr) return Error::NoMem;
                        _pcb = pcb;
                        attachTcp();
                        break;
                }
                case Type::Datagram: {
                        struct udp_pcb *pcb = udp_new();
                        if (pcb == nullptr) return Error::NoMem;
                        udp_recv(pcb, &SocketCallbacks::udpReceived, this);
                        _pcb = pcb;
                        break;
                }
                case Type::Raw: {
                        struct raw_pcb *pcb = raw_new(_protocol);
                        if (pcb == nullptr) return Error::NoMem;
                        raw_recv(pcb, &SocketCallbacks::rawReceived, this);
                        _pcb = pcb;
                        break;
                }
        }
        applyOptions();
        return Error::Ok;
}

void Socket::attachTcp() {
        auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
        tcp_arg(pcb, this);
        tcp_recv(pcb, &SocketCallbacks::tcpReceived);
        tcp_sent(pcb, &SocketCallbacks::tcpSent);
        tcp_err(pcb, &SocketCallbacks::tcpError);
        return;
}

void Socket::closePcb() {
        for (size_t i = 0; i < _groupCount; ++i) {
                const ip4_addr_t group = {_groups[i].group};
                const ip4_addr_t interface = {_groups[i].interface};
                (void)igmp_leavegroup(&interface, &group);
        }
        _groupCount = 0;
        switch (_type) {
                case Type::Stream:
                        if (_listening) {
                                if (_pcb != nullptr) {
                                        auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
                                        tcp_arg(pcb, nullptr);
                                        tcp_accept(pcb, nullptr);
                                        (void)tcp_close(pcb); // a listener's close always succeeds
                                }
                                for (size_t i = 0; i < _pendingCount; ++i) {
                                        _pending[i]->closePcb();
                                        _pending[i]->destroy();
                                }
                                _pendingCount = 0;
                        } else if (_pcb != nullptr) {
                                // Cut first: nothing reaches this socket again.
                                auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
                                tcp_arg(pcb, nullptr);
                                tcp_recv(pcb, nullptr); // lwIP's own then takes what comes, and the FIN
                                tcp_sent(pcb, nullptr);
                                tcp_err(pcb, nullptr);
                                // Unread data or a zero linger: a reset, as Linux does.
                                if (_receivedBytes > 0 || _linger == 0 || tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
                        }
                        break;
                case Type::Datagram:
                        if (_pcb != nullptr) udp_remove(static_cast<struct udp_pcb *>(_pcb));
                        break;
                case Type::Raw:
                        if (_pcb != nullptr) raw_remove(static_cast<struct raw_pcb *>(_pcb));
                        break;
        }
        _pcb = nullptr;
        dropReceived();
        return;
}

void Socket::dropReceived() {
        if (_received != nullptr) pbuf_free(static_cast<struct pbuf *>(_received));
        _received = nullptr;
        _receivedBytes = 0;
        for (; _datagramCount > 0; --_datagramCount) {
                pbuf_free(static_cast<struct pbuf *>(_datagrams[_datagramHead].pbuf));
                _datagramHead = (_datagramHead + 1) % MaxDatagrams;
        }
        _datagramBytes = 0;
        return;
}

void Socket::handleClosed(uint8_t mode) {
        (void)mode; // opened read and write: the one handle's last reference
        {
                NetStack::Locker lock;
                closePcb();
        }
        destroy();
        return;
}

void Socket::wakeAll() {
        _readable.release();
        _writable.release();
        _pollers.notify();
        return;
}

void Socket::queue(void *pbuf, uint32_t from, uint16_t port) {
        auto *p = static_cast<struct pbuf *>(pbuf);
        if (_receiveShut || _datagramCount == MaxDatagrams ||
            _datagramBytes + p->tot_len > static_cast<size_t>(_receiveBuffer)) {
                ++_dropped;
                pbuf_free(p);
                return;
        }
        p = keepable(p);
        if (p == nullptr) {
                ++_dropped;
                return;
        }
        Datagram &d = _datagrams[(_datagramHead + _datagramCount) % MaxDatagrams];
        d.pbuf = p;
        d.ipv4 = from;
        d.port = port;
        ++_datagramCount;
        _datagramBytes += p->tot_len;
        _readable.release();
        _pollers.notify();
        return;
}

// --- Options --------------------------------------------------------------------------------

void Socket::inheritOptions(const Socket &listener) {
        _reuseAddress = listener._reuseAddress;
        _keepAlive = listener._keepAlive;
        _noDelay = listener._noDelay;
        _keepIdle = listener._keepIdle;
        _keepInterval = listener._keepInterval;
        _keepCount = listener._keepCount;
        _ttl = listener._ttl;
        _tos = listener._tos;
        _linger = listener._linger;
        _receiveTimeoutMs.store(listener._receiveTimeoutMs.load());
        _sendTimeoutMs.store(listener._sendTimeoutMs.load());
        return;
}

void Socket::applyOptions() {
        if (_pcb == nullptr) return;
        auto *ip = static_cast<struct ip_pcb *>(_pcb);
        if (_reuseAddress) {
                ip_set_option(ip, SOF_REUSEADDR);
        } else {
                ip_reset_option(ip, SOF_REUSEADDR);
        }
        ip->ttl = _ttl;
        ip->tos = _tos;
        switch (_type) {
                case Type::Stream: {
                        if (_listening) break; // a listener's PCB has none of the rest
                        auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
                        if (_keepAlive) {
                                ip_set_option(ip, SOF_KEEPALIVE);
                        } else {
                                ip_reset_option(ip, SOF_KEEPALIVE);
                        }
                        pcb->keep_idle = static_cast<u32_t>(_keepIdle) * 1000;
                        pcb->keep_intvl = static_cast<u32_t>(_keepInterval) * 1000;
                        pcb->keep_cnt = static_cast<u32_t>(_keepCount);
                        if (_noDelay) {
                                tcp_nagle_disable(pcb);
                        } else {
                                tcp_nagle_enable(pcb);
                        }
                        break;
                }
                case Type::Datagram: {
                        auto *pcb = static_cast<struct udp_pcb *>(_pcb);
                        udp_set_multicast_ttl(pcb, _multicastTtl);
                        udp_setflags(pcb, _multicastLoop ? (udp_flags(pcb) | UDP_FLAGS_MULTICAST_LOOP)
                                                         : (udp_flags(pcb) & ~UDP_FLAGS_MULTICAST_LOOP));
                        struct netif *n = _multicastInterface != 0 ? netifWith(_multicastInterface) : nullptr;
                        udp_set_multicast_netif_index(pcb, n != nullptr ? netif_get_index(n) : NETIF_NO_INDEX);
                        break;
                }
                case Type::Raw: {
                        auto *pcb = static_cast<struct raw_pcb *>(_pcb);
                        raw_set_multicast_ttl(pcb, _multicastTtl);
                        raw_setflags(pcb, _multicastLoop ? (raw_flags(pcb) | RAW_FLAGS_MULTICAST_LOOP)
                                                         : (raw_flags(pcb) & ~RAW_FLAGS_MULTICAST_LOOP));
                        struct netif *n = _multicastInterface != 0 ? netifWith(_multicastInterface) : nullptr;
                        raw_set_multicast_netif_index(pcb, n != nullptr ? netif_get_index(n) : NETIF_NO_INDEX);
                        break;
                }
        }
        return;
}

Error Socket::setOption(Option option, int32_t value) {
        const bool       tcp = _type == Type::Stream;
        const bool       on = value != 0;
        NetStack::Locker lock;
        switch (option) {
                case Option::ReuseAddress: _reuseAddress = on; break;
                case Option::KeepAlive:
                        if (!tcp) return Error::NotSupported;
                        _keepAlive = on;
                        break;
                case Option::KeepIdle:
                case Option::KeepInterval:
                        if (!tcp) return Error::NotSupported;
                        if (value < 1 || value > 32767) return Error::Invalid;
                        (option == Option::KeepIdle ? _keepIdle : _keepInterval) = value;
                        break;
                case Option::KeepCount:
                        if (!tcp) return Error::NotSupported;
                        if (value < 1 || value > 127) return Error::Invalid;
                        _keepCount = value;
                        break;
                case Option::NoDelay:
                        if (!tcp) return Error::NotSupported;
                        _noDelay = on;
                        break;
                case Option::ReceiveBuffer:
                        // TCP's window is fixed: taken and kept as it is.
                        if (!tcp) {
                                _receiveBuffer = value < MinReceiveBuffer   ? MinReceiveBuffer
                                                 : value > MaxReceiveBuffer ? MaxReceiveBuffer
                                                                            : value;
                        }
                        break;
                case Option::SendBuffer: break; // fixed
                case Option::ReceiveTimeoutMs:
                        if (value < 0) return Error::Invalid;
                        _receiveTimeoutMs.store(value);
                        break;
                case Option::SendTimeoutMs:
                        if (value < 0) return Error::Invalid;
                        _sendTimeoutMs.store(value);
                        break;
                case Option::Broadcast: _broadcast = on; break;
                case Option::Linger: _linger = value < 0 ? -1 : value; break;
                case Option::Ttl:
                        if (value < 1 || value > 255) return Error::Invalid;
                        _ttl = static_cast<uint8_t>(value);
                        break;
                case Option::Tos:
                        if (value < 0 || value > 255) return Error::Invalid;
                        _tos = static_cast<uint8_t>(value);
                        break;
                case Option::MulticastTtl:
                        if (tcp) return Error::NotSupported;
                        if (value < 0 || value > 255) return Error::Invalid;
                        _multicastTtl = static_cast<uint8_t>(value);
                        break;
                case Option::MulticastLoop:
                        if (tcp) return Error::NotSupported;
                        _multicastLoop = on;
                        break;
                case Option::Listening:
                case Option::MaxSegment: return Error::Invalid;
        }
        applyOptions();
        return Error::Ok;
}

Result<int32_t> Socket::option(Option option) {
        const bool            tcp = _type == Type::Stream;
        NetStack::QuietLocker lock;
        switch (option) {
                case Option::ReuseAddress: return makeResult<int32_t>(_reuseAddress);
                case Option::KeepAlive: return makeResult<int32_t>(_keepAlive);
                case Option::KeepIdle: return makeResult(_keepIdle);
                case Option::KeepInterval: return makeResult(_keepInterval);
                case Option::KeepCount: return makeResult(_keepCount);
                case Option::NoDelay: return makeResult<int32_t>(_noDelay);
                case Option::ReceiveBuffer: return makeResult<int32_t>(tcp ? TCP_WND : _receiveBuffer);
                case Option::SendBuffer: return makeResult<int32_t>(tcp ? TCP_SND_BUF : MaxUdpPayload);
                case Option::ReceiveTimeoutMs: return makeResult(_receiveTimeoutMs.load());
                case Option::SendTimeoutMs: return makeResult(_sendTimeoutMs.load());
                case Option::Broadcast: return makeResult<int32_t>(_broadcast);
                case Option::Linger: return makeResult(_linger);
                case Option::Ttl: return makeResult<int32_t>(_ttl);
                case Option::Tos: return makeResult<int32_t>(_tos);
                case Option::MulticastTtl:
                        if (tcp) return makeError<int32_t>(Error::NotSupported);
                        return makeResult<int32_t>(_multicastTtl);
                case Option::MulticastLoop:
                        if (tcp) return makeError<int32_t>(Error::NotSupported);
                        return makeResult<int32_t>(_multicastLoop);
                case Option::Listening: return makeResult<int32_t>(_listening);
                case Option::MaxSegment:
                        if (!tcp) return makeError<int32_t>(Error::NotSupported);
                        return makeResult<int32_t>(TCP_MSS);
        }
        return makeError<int32_t>(Error::NotSupported);
}

Error Socket::takeError() {
        NetStack::QuietLocker lock;
        const Error           err = _error;
        _error = Error::Ok;
        return err;
}

Error Socket::joinGroup(uint32_t group, uint32_t interface) {
        if (_type == Type::Stream) return Error::NotSupported;
        const ip4_addr_t g = {group};
        const ip4_addr_t i = {interface};
        if (!ip4_addr_ismulticast(&g)) return Error::Invalid;
        NetStack::Locker lock;
        for (size_t k = 0; k < _groupCount; ++k) {
                if (_groups[k].group == group && _groups[k].interface == interface) return Error::Exists;
        }
        if (_groupCount == MaxGroups) return Error::Full;
        if (interface != 0 && netifWith(interface) == nullptr) return Error::AddressNotAvailable;
        const err_t err = igmp_joingroup(&i, &g);
        if (err != ERR_OK) return err == ERR_VAL ? Error::AddressNotAvailable : errorOf(err);
        _groups[_groupCount++] = Group{group, interface};
        return Error::Ok;
}

Error Socket::leaveGroup(uint32_t group, uint32_t interface) {
        NetStack::Locker lock;
        for (size_t k = 0; k < _groupCount; ++k) {
                if (_groups[k].group != group || _groups[k].interface != interface) continue;
                const ip4_addr_t g = {group};
                const ip4_addr_t i = {interface};
                (void)igmp_leavegroup(&i, &g);
                _groups[k] = _groups[--_groupCount];
                return Error::Ok;
        }
        return Error::NotExist;
}

Error Socket::setMulticastInterface(uint32_t interface) {
        if (_type == Type::Stream) return Error::NotSupported;
        NetStack::Locker lock;
        if (interface != 0 && netifWith(interface) == nullptr) return Error::AddressNotAvailable;
        _multicastInterface = interface;
        applyOptions();
        return Error::Ok;
}

uint32_t Socket::multicastInterface() {
        NetStack::QuietLocker lock;
        return _multicastInterface;
}

Error Socket::bindToDevice(StringView name) {
        char text[16] = {};
        if (name.size() >= sizeof(text)) return Error::NotExist;
        for (size_t i = 0; i < name.size(); ++i) text[i] = name[i];
        NetStack::Locker lock;
        struct netif    *n = nullptr;
        if (!name.isEmpty()) {
                n = bmds_netif_named(text);
                if (n == nullptr) return Error::NotExist;
        }
        if (_pcb == nullptr) return Error::Ok;
        switch (_type) {
                case Type::Stream:
                        if (!_listening) tcp_bind_netif(static_cast<struct tcp_pcb *>(_pcb), n);
                        break;
                case Type::Datagram: udp_bind_netif(static_cast<struct udp_pcb *>(_pcb), n); break;
                case Type::Raw: raw_bind_netif(static_cast<struct raw_pcb *>(_pcb), n); break;
        }
        return Error::Ok;
}

// --- Addresses ------------------------------------------------------------------------------

Error Socket::bind(const SocketAddress &address) {
        NetStack::Locker lock;
        if (_pcb == nullptr) return _gone ? Error::Invalid : Error::NoMem;
        if (!isBindable(address.ipv4, _type != Type::Stream)) return Error::AddressNotAvailable;
        const ip_addr_t ip = ipOf(address.ipv4);
        err_t           err = ERR_OK;
        switch (_type) {
                case Type::Stream: {
                        auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
                        if (_listening || _connecting || _connected || pcb->local_port != 0) return Error::Invalid;
                        err = tcp_bind(pcb, &ip, address.port);
                        break;
                }
                case Type::Datagram: {
                        auto *pcb = static_cast<struct udp_pcb *>(_pcb);
                        if (pcb->local_port != 0) return Error::Invalid;
                        err = udp_bind(pcb, &ip, address.port);
                        break;
                }
                case Type::Raw: err = raw_bind(static_cast<struct raw_pcb *>(_pcb), &ip); break;
        }
        return errorOf(err);
}

SocketAddress Socket::address() {
        NetStack::QuietLocker lock;
        SocketAddress         a;
        if (_pcb == nullptr) return a;
        a.ipv4 = ip_addr_get_ip4_u32(&static_cast<struct ip_pcb *>(_pcb)->local_ip);
        switch (_type) {
                case Type::Stream: a.port = static_cast<struct tcp_pcb *>(_pcb)->local_port; break;
                case Type::Datagram: a.port = static_cast<struct udp_pcb *>(_pcb)->local_port; break;
                case Type::Raw: break;
        }
        return a;
}

Result<SocketAddress> Socket::peer() {
        NetStack::QuietLocker lock;
        if (_pcb == nullptr || !_connected) return makeError<SocketAddress>(Error::NotConnected);
        SocketAddress a;
        a.ipv4 = ip_addr_get_ip4_u32(&static_cast<struct ip_pcb *>(_pcb)->remote_ip);
        switch (_type) {
                case Type::Stream: a.port = static_cast<struct tcp_pcb *>(_pcb)->remote_port; break;
                case Type::Datagram: a.port = static_cast<struct udp_pcb *>(_pcb)->remote_port; break;
                case Type::Raw: break;
        }
        return makeResult(a);
}

// --- Connections ----------------------------------------------------------------------------

unsigned int Socket::timeoutOf(const std::atomic<int32_t> &optionMs, unsigned int timeoutMs) const {
        // 0 is for ever in both: the shorter of the two that aren't.
        const auto option = static_cast<unsigned int>(optionMs.load(std::memory_order_relaxed));
        if (option == 0) return timeoutMs;
        if (timeoutMs == WaitForever) return option;
        return option < timeoutMs ? option : timeoutMs;
}

Error Socket::listen(int backlog) {
        if (_type != Type::Stream) return Error::NotSupported;
        const auto       n = static_cast<u8_t>(backlog < 1 ? 1 : backlog > int(MaxBacklog) ? int(MaxBacklog) : backlog);
        NetStack::Locker lock;
        if (_pcb == nullptr) return Error::Invalid;
        auto *pcb = static_cast<struct tcp_pcb *>(_pcb);
        if (_listening) {
                tcp_backlog_set(pcb, n);
                return Error::Ok;
        }
        if (_connecting || _connected) return Error::Invalid;
        if (pcb->local_port == 0) {
                const err_t err = tcp_bind(pcb, IP4_ADDR_ANY, 0);
                if (err != ERR_OK) return errorOf(err);
        }
        err_t           err = ERR_OK;
        struct tcp_pcb *listener = tcp_listen_with_backlog_and_err(pcb, n, &err);
        if (listener == nullptr) return errorOf(err);
        _pcb = listener; // the first PCB is freed
        _listening = true;
        tcp_arg(listener, this);
        tcp_accept(listener, &SocketCallbacks::tcpAccepted);
        return Error::Ok;
}

Result<FileHandle::Ptr> Socket::accept(SocketAddress *peer, uint32_t flags) {
        const Deadline deadline(timeoutOf(_receiveTimeoutMs, WaitForever));
        if (_readLock.lock(deadline).isError()) return makeError<FileHandle::Ptr>(Error::WouldBlock);
        Socket *c = nullptr;
        Error   err;
        for (;;) {
                {
                        NetStack::Locker lock;
                        if (!_listening) {
                                err = Error::Invalid;
                                break;
                        }
                        if (_pendingCount > 0) {
                                c = _pending[0];
                                for (size_t i = 1; i < _pendingCount; ++i) _pending[i - 1] = _pending[i];
                                --_pendingCount;
                                if (c->_pcb != nullptr) tcp_backlog_accepted(static_cast<struct tcp_pcb *>(c->_pcb));
                                break;
                        }
                }
                if (flags & DontWait) {
                        err = Error::WouldBlock;
                        break;
                }
                err = Interrupt::waitCurrent(_readable, deadline);
                if (err.isError()) {
                        if (err == Error::Timeout) err = Error::WouldBlock;
                        break;
                }
        }
        _readLock.unlock();
        if (c == nullptr) return makeError<FileHandle::Ptr>(err);
        if (peer != nullptr) {
                auto [a, perr] = c->peer();
                *peer = perr.isOk() ? a : SocketAddress{};
        }
        auto [handle, herr] = FileHandle::open(*c, FileHandle::ReadWrite);
        if (herr.isError()) {
                {
                        NetStack::Locker lock;
                        c->closePcb();
                }
                c->destroy();
                return makeError<FileHandle::Ptr>(herr);
        }
        return makeResult(std::move(handle));
}

Error Socket::connect(const SocketAddress &address, uint32_t flags) {
        const ip_addr_t ip = ipOf(address.ipv4);
        if (_type != Type::Stream) {
                NetStack::Locker lock;
                if (_pcb == nullptr) return Error::NoMem;
                if (address.ipv4 == 0 && address.port == 0) {
                        if (_type == Type::Datagram) {
                                udp_disconnect(static_cast<struct udp_pcb *>(_pcb));
                        } else {
                                raw_disconnect(static_cast<struct raw_pcb *>(_pcb));
                        }
                        _connected = false;
                        return Error::Ok;
                }
                const err_t err = _type == Type::Datagram
                                          ? udp_connect(static_cast<struct udp_pcb *>(_pcb), &ip, address.port)
                                          : raw_connect(static_cast<struct raw_pcb *>(_pcb), &ip);
                if (err == ERR_OK) _connected = true;
                return errorOf(err);
        }
        const Deadline deadline(timeoutOf(_sendTimeoutMs, WaitForever));
        {
                NetStack::Locker lock;
                if (_listening) return Error::Invalid;
                if (_connected) return Error::AlreadyConnected;
                if (_connecting) {
                        if (flags & DontWait) return Error::AlreadyInProgress;
                } else {
                        if (_pcb == nullptr) {
                                const Error pending = _error;
                                _error = Error::Ok;
                                return pending.isError() ? pending : Error::Invalid;
                        }
                        if (address.ipv4 == 0 || address.port == 0) return Error::Invalid;
                        const err_t err = tcp_connect(static_cast<struct tcp_pcb *>(_pcb), &ip, address.port,
                                                      &SocketCallbacks::tcpConnected);
                        if (err != ERR_OK) return errorOf(err);
                        _connecting = true;
                        if (flags & DontWait) return Error::InProgress;
                }
        }
        for (;;) {
                {
                        NetStack::Locker lock;
                        if (_connected) return Error::Ok;
                        if (!_connecting) {
                                const Error pending = _error;
                                _error = Error::Ok;
                                return pending.isError() ? pending : Error::ConnectionAborted;
                        }
                }
                const Error err = Interrupt::waitCurrent(_writable, deadline);
                if (err == Error::Timeout) return Error::InProgress; // it goes on, as Linux's does
                if (err.isError()) return err;
        }
}

Error Socket::shutdown(bool receiving, bool sending) {
        NetStack::Locker lock;
        if (!_connected && !_listening) return Error::NotConnected;
        if (receiving && !_receiveShut) {
                _receiveShut = true;
                if (_type == Type::Stream && _pcb != nullptr && _receivedBytes > 0)
                        tcp_recved(static_cast<struct tcp_pcb *>(_pcb), static_cast<u16_t>(_receivedBytes));
                dropReceived();
        }
        if (sending && !_sendShut) {
                _sendShut = true;
                // The FIN; never both sides through lwIP, which would free the PCB.
                if (_type == Type::Stream && !_listening && _pcb != nullptr)
                        (void)tcp_shutdown(static_cast<struct tcp_pcb *>(_pcb), 0, 1);
        }
        wakeAll();
        return Error::Ok;
}

// --- Sending --------------------------------------------------------------------------------

Result<size_t> Socket::send(Span<const ByteSpan> pieces, const SocketAddress *to, uint32_t flags) {
        return sendWithin(pieces, to, flags, WaitForever);
}

Result<size_t> Socket::sendWithin(Span<const ByteSpan> pieces, const SocketAddress *to, uint32_t flags,
                                  unsigned int timeoutMs) {
        const Deadline deadline(timeoutOf(_sendTimeoutMs, timeoutMs));
        if (_writeLock.lock(deadline).isError()) return makeError<size_t>(Error::WouldBlock);
        Result<size_t> r;
        if (_type == Type::Stream) {
                r = sendTcp(pieces, flags, deadline);
        } else {
                NetStack::Locker lock;
                r = sendDatagram(pieces, to);
        }
        _writeLock.unlock();
        return r;
}

Result<size_t> Socket::sendTcp(Span<const ByteSpan> pieces, uint32_t flags, const Deadline &deadline) {
        const size_t total = totalOf(pieces);
        size_t       done = 0;
        for (;;) {
                {
                        NetStack::Locker lock;
                        if (_error.isError()) {
                                const Error err = _error;
                                _error = Error::Ok;
                                return Result<size_t>(done, err);
                        }
                        if (_sendShut || _gone) return Result<size_t>(done, Error::BrokenPipe);
                        if (!_connected || _pcb == nullptr) return Result<size_t>(done, Error::NotConnected);
                        auto  *pcb = static_cast<struct tcp_pcb *>(_pcb);
                        size_t piece = 0; // where `done` is: piece and offset in it
                        size_t offset = done;
                        while (piece < pieces.size() && offset >= pieces[piece].size())
                                offset -= pieces[piece++].size();
                        bool wrote = false;
                        while (done < total) {
                                const size_t room = tcp_sndbuf(pcb);
                                if (room == 0 || tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN) break;
                                const ByteSpan p = pieces[piece];
                                size_t         n = p.size() - offset;
                                if (n > room) n = room;
                                const bool last = piece + 1 == pieces.size() && offset + n == p.size();
                                const u8_t api =
                                        TCP_WRITE_FLAG_COPY | ((!last || (flags & More)) ? TCP_WRITE_FLAG_MORE : 0);
                                const err_t err = tcp_write(pcb, p.data() + offset, static_cast<u16_t>(n), api);
                                if (err == ERR_MEM) {
                                        // Nothing in flight to free room: no sent callback will come.
                                        if (!wrote && tcp_sndqueuelen(pcb) == 0)
                                                return Result<size_t>(done, Error::NoMem);
                                        break;
                                }
                                if (err != ERR_OK) return Result<size_t>(done, errorOf(err));
                                wrote = true;
                                done += n;
                                offset += n;
                                if (offset == p.size()) {
                                        ++piece;
                                        offset = 0;
                                }
                        }
                        if (wrote) (void)tcp_output(pcb);
                        if (done == total) return makeResult(done);
                        if (flags & DontWait) {
                                if (done > 0) return makeResult(done);
                                return makeError<size_t>(Error::WouldBlock);
                        }
                }
                const Error err = Interrupt::waitCurrent(_writable, deadline);
                if (err.isError()) return Result<size_t>(done, err == Error::Timeout ? Error::WouldBlock : err);
        }
}

Result<size_t> Socket::sendDatagram(Span<const ByteSpan> pieces, const SocketAddress *to) {
        const size_t total = totalOf(pieces);
        if (total > (_type == Type::Raw ? MaxRawPayload : MaxUdpPayload))
                return makeError<size_t>(Error::MessageTooLong);
        if (_sendShut) return makeError<size_t>(Error::BrokenPipe);
        if (_pcb == nullptr) return makeError<size_t>(Error::NoMem);
        if (to == nullptr && !_connected) return makeError<size_t>(Error::NotConnected);
        if (to != nullptr && _type == Type::Datagram && to->port == 0) return makeError<size_t>(Error::Invalid);
        struct pbuf *p = pbuf_alloc(_type == Type::Raw ? PBUF_IP : PBUF_TRANSPORT, static_cast<u16_t>(total), PBUF_RAM);
        if (p == nullptr) return makeError<size_t>(Error::NoMem);
        size_t at = 0;
        for (const ByteSpan &piece : pieces) {
                if (piece.isEmpty()) continue;
                (void)pbuf_take_at(p, piece.data(), static_cast<u16_t>(piece.size()), static_cast<u16_t>(at));
                at += piece.size();
        }
        err_t err;
        if (to != nullptr) {
                const ip_addr_t ip = ipOf(to->ipv4);
                err = _type == Type::Datagram ? udp_sendto(static_cast<struct udp_pcb *>(_pcb), p, &ip, to->port)
                                              : raw_sendto(static_cast<struct raw_pcb *>(_pcb), p, &ip);
        } else {
                err = _type == Type::Datagram ? udp_send(static_cast<struct udp_pcb *>(_pcb), p)
                                              : raw_send(static_cast<struct raw_pcb *>(_pcb), p);
        }
        pbuf_free(p);
        if (err != ERR_OK) return makeError<size_t>(errorOf(err));
        return makeResult(total);
}

// --- Receiving ------------------------------------------------------------------------------

Result<Socket::Received> Socket::receive(Span<const WritableByteSpan> pieces, uint32_t flags) {
        return receiveWithin(pieces, flags, WaitForever);
}

Result<Socket::Received> Socket::receiveWithin(Span<const WritableByteSpan> pieces, uint32_t flags,
                                               unsigned int timeoutMs) {
        const Deadline deadline(timeoutOf(_receiveTimeoutMs, timeoutMs));
        if (_readLock.lock(deadline).isError()) return makeError<Received>(Error::WouldBlock);
        const size_t     room = totalOf(pieces);
        const bool       all = _type == Type::Stream && (flags & WaitAll) != 0 && (flags & Peek) == 0;
        Received         got;
        Result<Received> r;
        for (;;) {
                Error err;
                {
                        NetStack::Locker lock;
                        auto [part, perr] = receiveLocked(pieces, flags, got.bytes);
                        err = perr;
                        if (err.isOk()) {
                                got.bytes += part.bytes;
                                got.length = _type == Type::Stream ? got.bytes : part.length;
                                got.from = part.from;
                        }
                }
                if (err.isOk() && (!all || got.bytes == room)) {
                        r = makeResult(got);
                        break;
                }
                if (err.isError() && err != Error::WouldBlock) {
                        // What WaitAll gathered before the end or an error is still received.
                        r = got.bytes > 0 ? makeResult(got) : makeError<Received>(err);
                        break;
                }
                if (err.isError() && (flags & DontWait)) {
                        r = got.bytes > 0 ? makeResult(got) : makeError<Received>(Error::WouldBlock);
                        break;
                }
                if (err.isError()) {
                        const Error w = Interrupt::waitCurrent(_readable, deadline);
                        if (w.isError()) {
                                r = got.bytes > 0 ? makeResult(got)
                                                  : makeError<Received>(w == Error::Timeout ? Error::WouldBlock : w);
                                break;
                        }
                }
        }
        _readLock.unlock();
        return r;
}

Result<Socket::Received> Socket::receiveLocked(Span<const WritableByteSpan> pieces, uint32_t flags, size_t skip) {
        const size_t room = totalOf(pieces) - skip;
        Received     r;
        if (_type == Type::Stream) {
                if (_receivedBytes == 0) {
                        if (_error.isError()) {
                                const Error err = _error;
                                if ((flags & Peek) == 0) _error = Error::Ok;
                                return makeError<Received>(err);
                        }
                        if (_peerFinished || _receiveShut || _gone) return makeError<Received>(Error::EndOfFile);
                        if (!_connected && !_connecting) return makeError<Received>(Error::NotConnected);
                        if (room == 0) return makeResult(r);
                        return makeError<Received>(Error::WouldBlock);
                }
                const size_t n = room < _receivedBytes ? room : _receivedBytes;
                auto        *chain = static_cast<struct pbuf *>(_received);
                copyOut(chain, 0, pieces, skip, n);
                if ((flags & Peek) == 0) {
                        if (n == _receivedBytes) {
                                pbuf_free(chain);
                                _received = nullptr;
                        } else {
                                _received = pbuf_free_header(chain, static_cast<u16_t>(n));
                        }
                        _receivedBytes -= n;
                        if (_pcb != nullptr) tcp_recved(static_cast<struct tcp_pcb *>(_pcb), static_cast<u16_t>(n));
                }
                r.bytes = n;
                r.length = n;
                if (_pcb != nullptr) {
                        r.from.ipv4 = ip_addr_get_ip4_u32(&static_cast<struct tcp_pcb *>(_pcb)->remote_ip);
                        r.from.port = static_cast<struct tcp_pcb *>(_pcb)->remote_port;
                }
                return makeResult(r);
        }
        if (_datagramCount == 0) {
                if (_receiveShut) return makeError<Received>(Error::EndOfFile);
                return makeError<Received>(Error::WouldBlock);
        }
        Datagram    &d = _datagrams[_datagramHead];
        auto        *p = static_cast<struct pbuf *>(d.pbuf);
        const size_t length = p->tot_len;
        const size_t n = room < length ? room : length;
        copyOut(p, 0, pieces, skip, n);
        r.bytes = n;
        r.length = length;
        r.from.ipv4 = d.ipv4;
        r.from.port = d.port;
        if ((flags & Peek) == 0) {
                pbuf_free(p);
                d.pbuf = nullptr;
                _datagramHead = (_datagramHead + 1) % MaxDatagrams;
                --_datagramCount;
                _datagramBytes -= length;
        }
        return makeResult(r);
}

// --- As a Stream ----------------------------------------------------------------------------

Result<size_t> Socket::read(WritableByteSpan buf, unsigned int timeoutMs) {
        const WritableByteSpan pieces[1] = {buf};
        auto [r, err] = receiveWithin(Span<const WritableByteSpan>(pieces, 1), 0, timeoutMs);
        if (err.isError()) return Result<size_t>(0, err);
        return makeResult(r.bytes);
}

Result<size_t> Socket::write(ByteSpan data, unsigned int timeoutMs) {
        const ByteSpan pieces[1] = {data};
        return sendWithin(Span<const ByteSpan>(pieces, 1), nullptr, 0, timeoutMs);
}

bool Socket::isReadable() const {
        if (_error.isError()) return true;
        if (_listening) return _pendingCount > 0;
        if (_type == Type::Stream) return _receivedBytes > 0 || _peerFinished || _receiveShut || _gone;
        return _datagramCount > 0 || _receiveShut;
}

bool Socket::isWritable() const {
        if (_type != Type::Stream) return true;
        if (_listening || _connecting) return false;
        if (_gone || _sendShut || _error.isError()) return true; // a write fails at once
        if (!_connected || _pcb == nullptr) return false;
        const auto *pcb = static_cast<const struct tcp_pcb *>(_pcb);
        return tcp_sndbuf(pcb) > 0 && tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN;
}

uint8_t Socket::readiness() {
        NetStack::QuietLocker lock;
        uint8_t               ready = 0;
        if (isReadable()) ready |= Readable;
        if (isWritable()) ready |= Writable;
        if (_type == Type::Stream && !_listening) {
                // Reset or failed: everything at once, as Linux's poll says. Both sides
                // finished: hung up.
                if (_gone) ready |= Readable | Writable | HungUp | Broken;
                if (_peerFinished && _sendShut) ready |= HungUp;
        }
        return ready;
}

BMDS_NAMESPACE_END
