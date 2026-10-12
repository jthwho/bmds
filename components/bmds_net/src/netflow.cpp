/**
 * @file      netflow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Real-time flows (devplan/network.md §30d).
 */

#include <cstring>
#include <lwip/etharp.h>
#include <lwip/igmp.h>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/netflow.h>
#include <bmds/netstack.h>
#include <bmds/semaphore.h>
#include <bmds/system.h>

#include "netinterface.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // A destination's MAC asked again this often while a flow sends (ARP answers are snooped).
        constexpr uint64_t ArpRefreshUs = 30'000'000;

        // connect() asks this often until answered.
        constexpr unsigned int ArpRetryMs = 250;

        // An ARP request waits this long for a descriptor (it goes as a real-time frame).
        constexpr unsigned int ArpSendTimeoutMs = 5;

        ip4_addr_t toLwip(uint32_t hostOrder) {
                ip4_addr_t a;
                ip4_addr_set_u32(&a, lwip_htonl(hostOrder));
                return a;
        }

        uint32_t fromLwip(const ip4_addr_t *a) {
                return lwip_ntohl(ip4_addr_get_u32(a));
        }

        // Waits on @p list until @p ready gives anything but WouldBlock.
        template <typename T, typename F> Result<T> waitFor(WaitList &list, unsigned int timeoutMs, F ready) {
                const Deadline  deadline(timeoutMs);
                Semaphore       woken(1, 0);
                WaitList::Entry entry(list, woken);
                for (;;) {
                        Result<T> r = ready();
                        if (r.error() != Error::WouldBlock) return r;
                        if (!entry.isAdded()) return makeError<T>(Error::Busy);
                        if (woken.acquire(deadline).isError()) return makeError<T>(Error::Timeout);
                }
        }

} // namespace

Error NetFlow::open(EthernetDevice &device, const Config &config) {
        using NetClassifier::Kind;
        if (isOpen()) return Error::Busy;
        if (config.depth < 1 || config.depth > MaxDepth) return Error::Invalid;
        NetClassifier::Rule rule;
        rule.kind = config.kind;
        NetInterface *interface = NetInterface::find(device.name());
        if (config.kind == Kind::Udp) {
                if (config.port == 0 || config.dscp > 63) return Error::Invalid;
                if (interface == nullptr) return Error::NotReady;
                rule.port = config.port;
                rule.address = config.address;
        } else if (config.kind == Kind::EtherType) {
                using Ethernet::EtherType;
                const uint16_t t = config.etherType;
                if (t < 0x0600 || t == static_cast<uint16_t>(EtherType::Ipv4) ||
                    t == static_cast<uint16_t>(EtherType::Arp) || t == static_cast<uint16_t>(EtherType::Vlan))
                        return Error::Invalid;
                if (!config.group.isZero() && !config.group.isMulticast()) return Error::Invalid;
                rule.etherType = t;
        } else {
                return Error::Invalid;
        }
        {
                SpinLock::Locker lock(_lock);
                _config = config;
                if (config.kind != Kind::EtherType) _config.group = MacAddress{};
                _head = _count = _held = 0;
                _stampHead = _stampCount = 0;
                _counters = Counters{};
                _connected = false;
                _nextHop = 0;
                _endpoints = NetClassifier::UdpEndpoints{};
                _endpoints.sourcePort = config.port;
                _endpoints.ttl = config.ttl;
                _multicastTtl = config.ttl;
                _endpoints.dscp = config.dscp;
        }
        _device = &device;
        _interface = interface;
        _rule = rule;
        Error err = device.addFlow(*this, rule, _config.group);
        if (err.isError()) return err;
        if (config.kind == Kind::Udp && NetClassifier::isGroup(config.address)) {
                // The switch forwards a group to the ports that asked for it.
                const ip4_addr_t g = toLwip(config.address);
                err_t            e;
                {
                        NetStack::Locker lock;
                        e = igmp_joingroup_netif(&interface->netif(), &g);
                }
                if (e != ERR_OK) {
                        device.removeFlow(*this);
                        return Error::NoMem;
                }
        }
        _open.store(true, std::memory_order_release);
        return Error::Ok;
}

void NetFlow::close() {
        if (!isOpen()) return;
        _device->removeFlow(*this);
        if (_config.kind == NetClassifier::Kind::Udp && NetClassifier::isGroup(_config.address)) {
                const ip4_addr_t g = toLwip(_config.address);
                NetStack::Locker lock;
                (void)igmp_leavegroup_netif(&_interface->netif(), &g);
        }
        _open.store(false, std::memory_order_release);
        for (;;) {
                Packet p;
                {
                        SpinLock::Locker lock(_lock);
                        if (_count == 0) break;
                        p = _ring[_head];
                        _head = (_head + 1) % MaxDepth;
                        --_count;
                }
                release(p);
        }
        _waiters.notify(); // a waiter sees it closed
        tell();
        return;
}

void NetFlow::tell() {
        if (_listener != nullptr) _listener(_listenerArg);
        return;
}

bool NetFlow::deliver(const EthernetDevice::RxFrame &frame, const NetClassifier::Match &match) {
        {
                SpinLock::Locker lock(_lock);
                if (_held >= _config.depth) {
                        ++_counters.rxDropped;
                        return false;
                }
                Packet &p = _ring[(_head + _count) % MaxDepth];
                p.frame = frame;
                p.offset = match.offset;
                p.length = match.length;
                p.sourceIp = match.sourceIp;
                p.sourcePort = match.sourcePort;
                p.source = match.source;
                p.softwareNs = Clock::now().ns;
                ++_count;
                ++_held;
                ++_counters.rxPackets;
        }
        _waiters.notify();
        tell();
        return true;
}

Result<NetFlow::Packet> NetFlow::tryReceive() {
        if (!isOpen()) return makeError<Packet>(Error::NotReady);
        SpinLock::Locker lock(_lock);
        if (_count == 0) return makeError<Packet>(Error::WouldBlock);
        const Packet p = _ring[_head];
        _head = (_head + 1) % MaxDepth;
        --_count;
        return makeResult(p);
}

Result<NetFlow::Packet> NetFlow::receive(unsigned int timeoutMs) {
        return waitFor<Packet>(_waiters, timeoutMs, [this] { return tryReceive(); });
}

void NetFlow::release(const Packet &packet) {
        {
                SpinLock::Locker lock(_lock);
                --_held;
        }
        _device->release(packet.frame);
        return;
}

size_t NetFlow::queued() const {
        SpinLock::Locker lock(_lock);
        return _count;
}

void NetFlow::stamped(uint32_t cookie, uint32_t ticket, const PtpTime &time) {
        (void)cookie;
        {
                SpinLock::Locker lock(_lock);
                if (_stampCount == StampDepth) { // the oldest goes
                        _stampHead = (_stampHead + 1) % StampDepth;
                        --_stampCount;
                        ++_counters.txStampsLost;
                }
                _stamps[(_stampHead + _stampCount) % StampDepth] = {ticket, time};
                ++_stampCount;
                ++_counters.txStamps;
        }
        _waiters.notify();
        tell();
        return;
}

size_t NetFlow::txStampsQueued() const {
        SpinLock::Locker lock(_lock);
        return _stampCount;
}

Result<NetFlow::TxStamp> NetFlow::tryReceiveTxStamp() {
        if (!isOpen()) return makeError<TxStamp>(Error::NotReady);
        SpinLock::Locker lock(_lock);
        if (_stampCount == 0) return makeError<TxStamp>(Error::WouldBlock);
        const TxStamp s = _stamps[_stampHead];
        _stampHead = (_stampHead + 1) % StampDepth;
        --_stampCount;
        return makeResult(s);
}

Result<NetFlow::TxStamp> NetFlow::receiveTxStamp(unsigned int timeoutMs) {
        return waitFor<TxStamp>(_waiters, timeoutMs, [this] { return tryReceiveTxStamp(); });
}

NetFlow::Counters NetFlow::counters() const {
        SpinLock::Locker lock(_lock);
        return _counters;
}

bool NetFlow::learn(uint32_t ip, const MacAddress &mac) {
        SpinLock::Locker lock(_lock);
        if (_nextHop == 0 || ip != _nextHop) return false;
        _endpoints.destinationMac = mac;
        const bool answered = !_connected; // connect() waits for it
        _connected = true;
        return answered;
}

void NetFlow::sendArpRequest(uint32_t sourceIp) {
        Arp::Packet p;
        p.op = Arp::Op::Request;
        p.senderMac = _device->address();
        p.senderIp = sourceIp;
        {
                SpinLock::Locker lock(_lock);
                p.targetIp = _nextHop;
                _arpAskedUs = System::uptimeUs();
        }
        uint8_t      frame[Ethernet::MinFrameBytes];
        const size_t n = Arp::writeFrame(p, Span<uint8_t>(frame, sizeof(frame)));
        (void)_device->transmit(Span<const uint8_t>(frame, n), EthernetDevice::TxRealTime, ArpSendTimeoutMs);
        return;
}

void NetFlow::route(uint32_t address, uint32_t &local, uint32_t &hop, MacAddress &mac, bool &unreachable) {
        uint32_t mask, gateway;
        hop = 0;
        mac = MacAddress{};
        {
                NetStack::QuietLocker lock;
                struct netif         &netif = _interface->netif();
                local = fromLwip(netif_ip4_addr(&netif));
                mask = fromLwip(netif_ip4_netmask(&netif));
                gateway = fromLwip(netif_ip4_gw(&netif));
                if (local != 0 && !NetClassifier::isGroup(address) && address != 0xffffffffu &&
                    address != (local | ~mask)) {
                        hop = ((address ^ local) & mask) == 0 ? address : gateway;
                        struct eth_addr  *eth = nullptr;
                        const ip4_addr_t *found = nullptr;
                        const ip4_addr_t  h = toLwip(hop);
                        if (hop != 0 && etharp_find_addr(&netif, &h, &eth, &found) >= 0 && eth != nullptr)
                                std::memcpy(mac.bytes, eth->addr, MacAddress::Bytes);
                }
        }
        unreachable = false;
        if (local == 0) return;
        if (NetClassifier::isGroup(address))
                mac = NetClassifier::groupMac(address);
        else if (address == 0xffffffffu || address == (local | ~mask))
                mac = MacAddress::broadcast();
        else if (hop == 0)
                unreachable = true;
        return;
}

MacAddress NetFlow::macFor(uint32_t address) {
        if (!isOpen() || _config.kind != NetClassifier::Kind::Udp || address == 0) return MacAddress{};
        uint32_t   local, hop;
        MacAddress mac;
        bool       unreachable;
        route(address, local, hop, mac, unreachable);
        return mac;
}

Error NetFlow::tryConnect(uint32_t address, uint16_t port) {
        if (!isOpen()) return Error::NotReady;
        if (_config.kind != NetClassifier::Kind::Udp || address == 0 || port == 0) return Error::Invalid;
        uint32_t   local, hop;
        MacAddress mac;
        bool       unreachable;
        route(address, local, hop, mac, unreachable);
        if (local == 0) return Error::NotReady;
        if (unreachable) return Error::NetworkUnreachable;
        {
                SpinLock::Locker lock(_lock);
                _endpoints.destinationIp = address;
                _endpoints.destinationPort = port;
                _endpoints.destinationMac = mac;
                _nextHop = hop;
                _connected = !mac.isZero();
                _arpAskedUs = mac.isZero() ? 0 : System::uptimeUs();
        }
        if (!mac.isZero()) return Error::Ok;
        // lwIP doesn't know it: asked here, the answer snooped (learn()).
        sendArpRequest(local);
        return Error::InProgress;
}

Error NetFlow::connect(uint32_t address, uint16_t port, unsigned int timeoutMs) {
        const Error first = tryConnect(address, port);
        if (first != Error::InProgress) return first;
        const uint32_t local = _interface->ipv4();
        Result<bool>   r = waitFor<bool>(_waiters, timeoutMs, [&] {
                bool due;
                {
                        SpinLock::Locker lock(_lock);
                        if (_connected) return makeResult(true);
                        due = System::uptimeUs() - _arpAskedUs >= ArpRetryMs * 1000ull;
                }
                if (due) sendArpRequest(local);
                return makeError<bool>(Error::WouldBlock);
        });
        if (r.isOk()) return Error::Ok;
        return r.error() == Error::Timeout ? Error::HostUnreachable : r.error();
}

void NetFlow::disconnect() {
        SpinLock::Locker lock(_lock);
        _connected = false;
        _nextHop = 0;
        _endpoints.destinationIp = 0;
        _endpoints.destinationPort = 0;
        _endpoints.destinationMac = MacAddress{};
        return;
}

Error NetFlow::filterSource(uint32_t address, uint16_t port) {
        if (!isOpen()) return Error::NotReady;
        if (_config.kind != NetClassifier::Kind::Udp) return Error::Invalid;
        _rule.sourceAddress = address;
        _rule.sourcePort = port;
        _device->setFlowRule(*this, _rule);
        return Error::Ok;
}

void NetFlow::setIpFields(uint8_t ttl, uint8_t multicastTtl, uint8_t dscp) {
        SpinLock::Locker lock(_lock);
        _endpoints.ttl = ttl;
        _endpoints.dscp = dscp & 63;
        _multicastTtl = multicastTtl;
        return;
}

size_t NetFlow::headerBytes() const {
        return _config.kind == NetClassifier::Kind::Udp ? NetClassifier::UdpHeadersBytes : Ethernet::HeaderBytes;
}

Error NetFlow::setDestination(const MacAddress &destination) {
        if (!isOpen()) return Error::NotReady;
        if (_config.kind != NetClassifier::Kind::EtherType || destination.isZero()) return Error::Invalid;
        SpinLock::Locker lock(_lock);
        _endpoints.destinationMac = destination;
        _connected = true;
        return Error::Ok;
}

Result<uint32_t> NetFlow::send(Span<const uint8_t> payload, uint32_t flags, unsigned int timeoutMs) {
        return send(Span<const Span<const uint8_t>>(&payload, 1), nullptr, flags, timeoutMs, nullptr);
}

Result<uint32_t> NetFlow::send(Span<const Span<const uint8_t>> pieces, const Destination *to, uint32_t flags,
                               unsigned int timeoutMs, uint8_t *headersOut) {
        // The headers and up to this many pieces of payload go to the driver as they are.
        constexpr size_t MaxPieces = 65;
        if (!isOpen()) return makeError<uint32_t>(Error::NotReady);
        if ((flags & ~static_cast<uint32_t>(Stamp)) != 0 || pieces.size() > MaxPieces - 1)
                return makeError<uint32_t>(Error::Invalid);
        const bool udp = _config.kind == NetClassifier::Kind::Udp;
        if (to != nullptr && (!udp || to->address == 0 || to->port == 0 || to->mac.isZero()))
                return makeError<uint32_t>(Error::Invalid);
        NetClassifier::UdpEndpoints e;
        uint16_t                    id;
        bool                        ask;
        {
                SpinLock::Locker lock(_lock);
                if (to == nullptr && !_connected) return makeError<uint32_t>(Error::NotConnected);
                e = _endpoints;
                id = ++_id;
                const uint32_t destination = to != nullptr ? to->address : e.destinationIp;
                if (NetClassifier::isGroup(destination)) e.ttl = _multicastTtl;
                ask = to == nullptr && _nextHop != 0 && System::uptimeUs() - _arpAskedUs >= ArpRefreshUs;
        }
        if (to != nullptr) {
                e.destinationIp = to->address;
                e.destinationPort = to->port;
                e.destinationMac = to->mac;
        }
        e.sourceMac = _device->address();
        size_t payloadBytes = 0;
        for (const Span<const uint8_t> &p : pieces) payloadBytes += p.size();
        uint8_t headers[NetClassifier::UdpHeadersBytes];
        size_t  headerBytes;
        if (udp) {
                e.sourceIp = _interface->ipv4();
                if (e.sourceIp == 0) return makeError<uint32_t>(Error::NotReady);
                headerBytes = NetClassifier::writeUdpHeaders(headers, e, pieces, id);
                if (headerBytes == 0) return makeError<uint32_t>(Error::MessageTooLong);
        } else {
                if (payloadBytes > Ethernet::MaxPayloadBytes) return makeError<uint32_t>(Error::MessageTooLong);
                headerBytes = Ethernet::writeHeader(headers, e.destinationMac, e.sourceMac, _config.etherType);
        }
        if (headersOut != nullptr) std::memcpy(headersOut, headers, headerBytes);
        Span<const uint8_t> frame[MaxPieces];
        size_t              count = 0;
        frame[count++] = Span<const uint8_t>(headers, headerBytes);
        for (const Span<const uint8_t> &p : pieces) {
                if (!p.isEmpty()) frame[count++] = p;
        }
        uint32_t sendFlags = EthernetDevice::TxRealTime;
        if (flags & Stamp) sendFlags |= EthernetDevice::TxStamp;
        Result<uint32_t> r = _device->send(Span<const Span<const uint8_t>>(frame, count), sendFlags, nullptr,
                                           timeoutMs, this);
        {
                SpinLock::Locker lock(_lock);
                if (r.isOk())
                        ++_counters.txPackets;
                else
                        ++_counters.txErrors;
        }
        // The destination's MAC asked again now and then (after the packet: it isn't late for it).
        if (ask) sendArpRequest(e.sourceIp);
        return r;
}

size_t NetFlow::list(EthernetDevice &device, Span<Info> out) {
        Mutex::Locker set(device._flowSetLock, WaitForever); // no flow goes meanwhile
        const size_t  n = device._flowCount.load(std::memory_order_relaxed);
        size_t        k = 0;
        for (; k < n && k < out.size(); ++k) {
                NetFlow         &f = *device._flows[k];
                SpinLock::Locker lock(f._lock);
                Info            &i = out[k];
                i.config = f._config;
                i.counters = f._counters;
                i.queued = f._count;
                i.destinationIp = f._connected ? f._endpoints.destinationIp : 0;
                i.destinationPort = f._connected ? f._endpoints.destinationPort : 0;
                i.destinationMac = f._connected ? f._endpoints.destinationMac : MacAddress{};
        }
        return k;
}

BMDS_NAMESPACE_END
