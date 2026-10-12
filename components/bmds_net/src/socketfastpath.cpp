/**
 * @file      socketfastpath.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A fast-path socket's NetFlow (devplan/network.md §30f).
 */

#include <cstring>
#include <new>
#include <lwip/netif.h>
#include <bmds/memory.h>
#include <bmds/netflow.h>
#include <bmds/netstack.h>

#include "netinterface.h"
#include "socketfastpath.h"

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t hostOrder(uint32_t networkOrder) {
                return lwip_ntohl(networkOrder);
        }

        size_t totalOf(Span<const ByteSpan> pieces) {
                size_t total = 0;
                for (const ByteSpan &p : pieces) total += p.size();
                return total;
        }

        class FlowPath final : public SocketFastPath {
                public:
                        void close() override {
                                if (_peeked) _flow.release(_peek);
                                _peeked = false;
                                _flow.close();
                                this->~FlowPath();
                                Memory::free(this);
                                return;
                        }

                        Result<Packet> receive(Span<const WritableByteSpan> pieces, bool peek) override {
                                NetFlow::Packet p;
                                if (_peeked) {
                                        p = _peek;
                                } else {
                                        auto [got, err] = _flow.tryReceive();
                                        if (err.isError()) return makeError<Packet>(Error::WouldBlock);
                                        p = got;
                                }
                                const ByteSpan payload = p.payload();
                                Packet         r;
                                for (const WritableByteSpan &piece : pieces) {
                                        if (r.bytes == payload.size()) break;
                                        size_t n = payload.size() - r.bytes;
                                        if (n > piece.size()) n = piece.size();
                                        std::memcpy(piece.data(), payload.data() + r.bytes, n);
                                        r.bytes += n;
                                }
                                r.length = payload.size();
                                r.from.ipv4 = lwip_htonl(p.sourceIp);
                                r.from.port = p.sourcePort;
                                r.hardwareNs = p.frame.stamped ? p.frame.stamp.toNs() : 0;
                                r.softwareNs = p.softwareNs;
                                if (peek) {
                                        _peek = p;
                                        _peeked = true;
                                } else {
                                        _peeked = false;
                                        _flow.release(p);
                                }
                                return makeResult(r);
                        }

                        bool readable() override { return _peeked || _flow.queued() > 0; }

                        void connect(const SocketAddress &peer) override {
                                if (peer.ipv4 == 0 && peer.port == 0) {
                                        _flow.disconnect();
                                        (void)_flow.filterSource(0, 0);
                                        return;
                                }
                                // Unanswered ARP: sends go through lwIP until the snooped answer.
                                (void)_flow.tryConnect(hostOrder(peer.ipv4), peer.port);
                                (void)_flow.filterSource(hostOrder(peer.ipv4), peer.port);
                                return;
                        }

                        Result<Sent> send(Span<const ByteSpan> pieces, const SocketAddress *to, bool stamp,
                                          uint8_t *headers, unsigned int timeoutMs) override {
                                if (totalOf(pieces) > NetClassifier::MaxUdpPayloadBytes)
                                        return makeError<Sent>(Error::NotSupported); // fragments: lwIP's
                                NetFlow::Destination  d;
                                NetFlow::Destination *dp = nullptr;
                                if (to != nullptr) {
                                        if (to->port == 0) return makeError<Sent>(Error::NotSupported);
                                        d.address = hostOrder(to->ipv4);
                                        d.port = to->port;
                                        d.mac = _flow.macFor(d.address);
                                        if (d.mac.isZero()) return makeError<Sent>(Error::NotSupported);
                                        dp = &d;
                                }
                                auto [ticket, err] = _flow.send(pieces, dp, stamp ? uint32_t(NetFlow::Stamp) : 0u,
                                                                timeoutMs, headers);
                                if (err == Error::NotConnected || err == Error::MessageTooLong)
                                        return makeError<Sent>(Error::NotSupported);
                                if (err.isError()) return makeError<Sent>(err);
                                Sent s;
                                s.ticket = ticket;
                                s.headerBytes = _flow.headerBytes();
                                return makeResult(s);
                        }

                        bool takeStamp(uint32_t &ticket, int64_t &hardwareNs) override {
                                auto [s, err] = _flow.tryReceiveTxStamp();
                                if (err.isError()) return false;
                                ticket = s.ticket;
                                hardwareNs = s.time.toNs();
                                return true;
                        }

                        bool hasStamp() override { return _flow.txStampsQueued() > 0; }

                        void setIpFields(uint8_t ttl, uint8_t multicastTtl, uint8_t tos) override {
                                _flow.setIpFields(ttl, multicastTtl, static_cast<uint8_t>(tos >> 2));
                                return;
                        }

                        NetFlow         _flow;
                        NetFlow::Packet _peek; // MSG_PEEK's, kept for the next receive
                        bool            _peeked = false;
        };

        // The interface a socket's flow goes on: its device's, its address's, or the first.
        NetInterface *interfaceFor(const SocketFastPath::Config &config) {
                NetStack::QuietLocker lock;
                struct netif         *want = nullptr;
                if (config.netifIndex != 0) {
                        want = netif_get_by_index(config.netifIndex);
                } else if (config.address != 0) {
                        struct netif *n;
                        NETIF_FOREACH(n) {
                                if (ip4_addr_get_u32(netif_ip4_addr(n)) == config.address) want = n;
                        }
                }
                for (size_t i = 0; i < NetInterface::count(); ++i) {
                        NetInterface &ni = NetInterface::at(i);
                        if (want == nullptr || &ni.netif() == want) return &ni;
                }
                return nullptr;
        }

} // namespace

Result<SocketFastPath *> SocketFastPath::open(const Config &config) {
        NetInterface *ni = interfaceFor(config);
        if (ni == nullptr) return makeError<SocketFastPath *>(Error::NotExist);
        void *block = Memory::alloc(MemSpace::Internal, sizeof(FlowPath));
        if (block == nullptr) return makeError<SocketFastPath *>(Error::NoMem);
        auto           *f = new (block) FlowPath();
        NetFlow::Config c;
        c.kind = NetClassifier::Kind::Udp;
        c.port = config.port;
        // Bound to one of ours, or a group, or any: broadcasts come to any only.
        c.address = hostOrder(config.address);
        c.packetUs = config.packetUs;
        c.depth = config.depth;
        c.ttl = config.ttl;
        c.dscp = static_cast<uint8_t>(config.tos >> 2);
        f->_flow.setListener(config.listener, config.listenerArg);
        const Error err = f->_flow.open(ni->device(), c);
        if (err.isError()) {
                f->~FlowPath();
                Memory::free(block);
                return makeError<SocketFastPath *>(err == Error::NotReady ? Error::NotExist : err);
        }
        f->_flow.setIpFields(config.ttl, config.multicastTtl, c.dscp);
        return makeResult<SocketFastPath *>(f);
}

BMDS_NAMESPACE_END
