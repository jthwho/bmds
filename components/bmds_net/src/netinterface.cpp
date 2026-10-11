/**
 * @file      netinterface.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An Ethernet device as an lwIP interface (devplan/network.md §6, §15).
 */

#include <cstdio>
#include <cstring>
#include <lwip/dhcp.h>
#include <lwip/etharp.h>
#include <lwip/igmp.h>
#include <lwip/ip4_addr.h>
#include <netif/ethernet.h>
#include <bmds/log.h>
#include <bmds/netstack.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>

#include "netchanges.h"
#include "netinterface.h"
#include "netnames.h"
#include "ntpclient.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<NetInterface> interfaces[NetInterface::MaxInterfaces];
        constinit size_t                     interfaceCount = 0;

        // Frames sent while the ring is full wait this long for a descriptor (16 frames drain in
        // 2 ms at 100 Mbit/s), then are dropped like a full queue's.
        constexpr unsigned int TxTimeoutMs = 20;

        // Pieces a frame may be gathered from: lwIP's chains are a header and a payload or two.
        constexpr size_t MaxPieces = 8;

        Error checkMac(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                MacAddress a;
                if (!MacAddress::fromString(value, a) || a.isMulticast() || a.isZero()) return Error::Invalid;
                return Error::Ok;
        }

        Error checkCidr(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                uint32_t a = 0;
                uint8_t  prefix = 0;
                if (!Ipv4::parseCidr(value, a, prefix) || prefix == 0) return Error::Invalid;
                return Error::Ok;
        }

        Error checkIpv4(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                uint32_t a = 0;
                return Ipv4::parse(value, a) ? Error::Ok : Error::Invalid;
        }

        ip4_addr_t toLwip(uint32_t hostOrder) {
                ip4_addr_t a;
                ip4_addr_set_u32(&a, lwip_htonl(hostOrder));
                return a;
        }

        uint32_t fromLwip(const ip4_addr_t *a) {
                return lwip_ntohl(ip4_addr_get_u32(a));
        }

} // namespace

Error NetInterface::create(EthernetDevice &device) {
        if (interfaceCount == MaxInterfaces) return Error::Full;
        NetInterface &n = interfaces[interfaceCount].create(device);
        Error         err = n.declareSettings();
        if (err.isError()) return err;
        if (netif_add(&n._netif, nullptr, nullptr, nullptr, &n, &NetInterface::netifInit, &ethernet_input) == nullptr)
                return Error::NoMem;
        netif_set_status_callback(&n._netif, &NetInterface::statusChanged);
        if (interfaceCount == 0) netif_set_default(&n._netif);
        ++interfaceCount;
        device.setLinkHandler(&NetInterface::linkChanged, &n);
        device.setReceiver(&NetInterface::received, &n);
        NetStack::wake(); // the settings and the link, applied by the task
        return Error::Ok;
}

size_t NetInterface::count() {
        return interfaceCount;
}

NetInterface &NetInterface::at(size_t index) {
        return *interfaces[index];
}

NetInterface *NetInterface::find(StringView name) {
        for (size_t i = 0; i < interfaceCount; ++i)
                if (StringView(interfaces[i]->name()) == name) return &*interfaces[i];
        return nullptr;
}

void NetInterface::pollAll() {
        for (size_t i = 0; i < interfaceCount; ++i) interfaces[i]->poll();
        return;
}

NetInterface::NetInterface(EthernetDevice &device) : _device(device) {
        for (RxPbuf &r : _rxPbufs) {
                r.owner = this;
                r.custom.custom_free_function = &NetInterface::freeRx;
        }
        return;
}

Error NetInterface::declareSettings() {
        struct Decl {
                        const char   *suffix;
                        Config::Type  type;
                        const char   *defaultValue;
                        const char   *help;
                        int32_t       min;
                        int32_t       max;
                        const char   *choices;
                        Config::Check check;
        };
        static constexpr Decl Decls[SettingCount] = {
                {"enabled", Config::Type::Bool, "true", "Use the interface", 0, 0, nullptr, nullptr},
                {"mac", Config::Type::String, "", "Its MAC address (empty: the chip's own)", 0,
                 static_cast<int32_t>(MacAddress::TextBytes), nullptr, &checkMac},
                {"mtu", Config::Type::Int, "1500", "The largest IP packet it sends", 576, 1500, nullptr, nullptr},
                {"ipv4/mode", Config::Type::Enum, "dhcp", "How it gets an IPv4 address", 0, 0, "dhcp|static|off",
                 nullptr},
                {"ipv4/address", Config::Type::String, "",
                 "Its IPv4 address with the prefix (192.168.1.50/24), for static", 0, 18, nullptr, &checkCidr},
                {"ipv4/gateway", Config::Type::String, "", "The IPv4 router, for static (empty: none)", 0,
                 static_cast<int32_t>(Ipv4::TextBytes), nullptr, &checkIpv4},
        };
        char prefix[Config::MaxKey + 1];
        std::snprintf(prefix, sizeof(prefix), "net/%s", name());
        for (size_t i = 0; i < SettingCount; ++i) {
                const Decl &d = Decls[i];
                char        key[sizeof(prefix) + 16]; // the longest suffix and more; declare() checks the length
                std::snprintf(key, sizeof(key), "%s/%s", prefix, d.suffix);
                Config::Param p;
                p.key = key;
                p.type = d.type;
                p.defaultValue = d.defaultValue;
                p.help = d.help;
                p.min = d.min;
                p.max = d.max;
                p.choices = d.choices;
                p.check = d.check;
                auto [id, err] = SysConf::declareKernel(p);
                if (err.isError()) {
                        bmdsErr("%s: setting %s not declared: %s", name(), key, err.desc());
                        return err;
                }
                _settings[i] = id;
        }
        Error err = _watcher.start(StringView(prefix), &NetInterface::settingsChanged, this);
        if (err.isOk())
                err = _hostnameWatcher.start(StringView("system/hostname"), &NetInterface::settingsChanged, this);
        return err;
}

err_t NetInterface::netifInit(struct netif *netif) {
        auto &n = *static_cast<NetInterface *>(netif->state);
        netif->name[0] = 'e';
        netif->name[1] = 't';
        netif->hwaddr_len = ETH_HWADDR_LEN;
        std::memcpy(netif->hwaddr, n._device.address().bytes, ETH_HWADDR_LEN);
        netif->mtu = Ethernet::MaxPayloadBytes;
        netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
        netif->output = &etharp_output;
        netif->linkoutput = &NetInterface::linkOutput;
        netif->hostname = n._hostname;
        netif_set_igmp_mac_filter(netif, &NetInterface::igmpFilter);
        return ERR_OK;
}

// --- Receiving ------------------------------------------------------------------------------

void NetInterface::received(void *arg, EthernetDevice &device, const EthernetDevice::RxFrame &frame) {
        auto &n = *static_cast<NetInterface *>(arg);
        bool  queued = false;
        {
                SpinLock::Locker lock(n._rxLock);
                if (n._rxCount < EthernetDevice::RxBuffers) {
                        n._rxQueue[(n._rxHead + n._rxCount) % EthernetDevice::RxBuffers] = frame;
                        ++n._rxCount;
                        queued = true;
                }
        }
        if (queued)
                NetStack::wake();
        else
                device.release(frame); // can't happen: each queued frame holds one of the buffers
        return;
}

void NetInterface::poll() {
        if (_settingsPending.exchange(false)) applySettings();
        if (_linkPending.exchange(false)) applyLink();
        for (;;) {
                EthernetDevice::RxFrame f;
                {
                        SpinLock::Locker lock(_rxLock);
                        if (_rxCount == 0) break;
                        f = _rxQueue[_rxHead];
                        _rxHead = (_rxHead + 1) % EthernetDevice::RxBuffers;
                        --_rxCount;
                }
                input(f);
        }
        return;
}

void NetInterface::input(const EthernetDevice::RxFrame &frame) {
        if (!netif_is_up(&_netif)) {
                _device.release(frame);
                ++_counters.rxDropped;
                return;
        }
        struct pbuf *p = nullptr;
        if (_held.load(std::memory_order_relaxed) >= MaxHeld) {
                // lwIP is holding buffers (TCP's queues): copy, so the ring keeps its own.
                p = pbuf_alloc(PBUF_RAW, frame.length, PBUF_RAM);
                if (p != nullptr) (void)pbuf_take(p, frame.data, frame.length);
                _device.release(frame);
                if (p == nullptr) {
                        ++_counters.rxDropped;
                        return;
                }
                ++_counters.rxCopied;
        } else {
                RxPbuf &r = _rxPbufs[frame.buffer];
                r.frame = frame;
                p = pbuf_alloced_custom(PBUF_RAW, frame.length, PBUF_REF, &r.custom, frame.data,
                                        EthernetDevice::BufferBytes);
                _held.fetch_add(1, std::memory_order_relaxed);
        }
        if (_netif.input(p, &_netif) != ERR_OK) pbuf_free(p);
        return;
}

void NetInterface::freeRx(struct pbuf *p) {
        auto *r = reinterpret_cast<RxPbuf *>(p);
        r->owner->_held.fetch_sub(1, std::memory_order_relaxed);
        r->owner->_device.release(r->frame);
        return;
}

// --- Sending --------------------------------------------------------------------------------

err_t NetInterface::linkOutput(struct netif *netif, struct pbuf *p) {
        auto               &n = *static_cast<NetInterface *>(netif->state);
        Span<const uint8_t> pieces[MaxPieces];
        size_t              count = 0;
        for (struct pbuf *q = p; q != nullptr; q = q->next) {
                if (q->len == 0) continue;
                if (count == MaxPieces) {
                        ++n._counters.txDropped;
                        return ERR_MEM;
                }
                pieces[count++] = Span<const uint8_t>(static_cast<const uint8_t *>(q->payload), q->len);
        }
        auto [ticket, err] = n._device.transmit(Span<const Span<const uint8_t>>(pieces, count), 0, TxTimeoutMs);
        (void)ticket;
        if (err.isError()) {
                ++n._counters.txDropped;
                return err == Error::NotReady ? ERR_IF : ERR_MEM;
        }
        return ERR_OK;
}

// --- The link, the settings, the groups -----------------------------------------------------

void NetInterface::linkChanged(void *arg, EthernetDevice &) {
        static_cast<NetInterface *>(arg)->_linkPending.store(true);
        NetStack::wake();
        return;
}

void NetInterface::applyLink() {
        if (_device.link().up)
                netif_set_link_up(&_netif);
        else
                netif_set_link_down(&_netif);
        NetStatus::noteChange();
        return;
}

void NetInterface::settingsChanged(void *arg) {
        static_cast<NetInterface *>(arg)->_settingsPending.store(true);
        NetStack::wake();
        return;
}

void NetInterface::applySettings() {
        SysConf::Id id;
        while (_watcher.next(id)) {}
        (void)_watcher.overflowed();
        while (_hostnameWatcher.next(id)) {}
        (void)_hostnameWatcher.overflowed();
        char was[sizeof(_hostname)];
        std::memcpy(was, _hostname, sizeof(was));
        (void)SysConf::hostname(_hostname);
        _applying = true; // the address is logged once, at the end
        const Mode wasMode = _mode;
        apply(wasMode);
        _applying = false;
        // A new name renews the lease at once: the DHCP server (and the DNS it feeds) learns it.
        if (_mode == Mode::Dhcp && wasMode == Mode::Dhcp && std::strcmp(was, _hostname) != 0 &&
            dhcp_supplied_address(&_netif))
                (void)dhcp_renew(&_netif);
        logAddress();
        NetStatus::noteChange();
        return;
}

void NetInterface::apply(Mode was) {
        // The station address.
        String     text;
        MacAddress mac = _device.factoryAddress();
        if (SysConf::get(_settings[Mac], text).isOk() && !text.isEmpty()) (void)MacAddress::fromString(text, mac);
        if (!(mac == _device.address())) {
                // A lease belongs to the old address: released, and asked for again below.
                if (was == Mode::Dhcp) {
                        dhcp_release_and_stop(&_netif);
                        was = Mode::Off;
                }
                if (_device.setAddress(mac).isOk()) {
                        std::memcpy(_netif.hwaddr, mac.bytes, ETH_HWADDR_LEN);
                        if (netif_is_up(&_netif)) etharp_gratuitous(&_netif);
                        char shown[MacAddress::TextBytes + 1];
                        mac.toString(shown);
                        bmdsInfo("%s: MAC address %s", name(), shown);
                }
        }

        auto [mtu, mtuErr] = SysConf::getInt(_settings[Mtu]);
        if (mtuErr.isOk()) _netif.mtu = static_cast<u16_t>(mtu);

        Mode mode = Mode::Dhcp;
        if (SysConf::get(_settings[Ipv4Mode], text).isOk()) {
                if (text.view() == StringView("static")) mode = Mode::Static;
                if (text.view() == StringView("off")) mode = Mode::Off;
        }
        auto [enabled, enabledErr] = SysConf::getBool(_settings[Enabled]);
        if (enabledErr.isOk() && !enabled) mode = Mode::Off;

        _mode = mode;
        if (mode != Mode::Dhcp && was == Mode::Dhcp) dhcp_release_and_stop(&_netif);
        const ip4_addr_t any = toLwip(0);
        if (mode == Mode::Off) {
                netif_set_addr(&_netif, &any, &any, &any);
                netif_set_down(&_netif);
                return;
        }
        if (mode == Mode::Static) {
                uint32_t address = 0;
                uint8_t  prefix = 0;
                uint32_t gateway = 0;
                if (SysConf::get(_settings[Ipv4Address], text).isError() || text.isEmpty() ||
                    !Ipv4::parseCidr(text, address, prefix)) {
                        bmdsWarn("%s: static IPv4 without an address (net/%s/ipv4/address)", name(), name());
                        address = 0;
                        prefix = 0;
                }
                if (SysConf::get(_settings[Ipv4Gateway], text).isOk() && !text.isEmpty())
                        (void)Ipv4::parse(text, gateway);
                const ip4_addr_t a = toLwip(address);
                const ip4_addr_t m = toLwip(Ipv4::maskOf(prefix));
                const ip4_addr_t g = toLwip(gateway);
                netif_set_addr(&_netif, &a, &m, &g);
        }
        netif_set_up(&_netif);
        if (mode == Mode::Dhcp && was != Mode::Dhcp) {
                netif_set_addr(&_netif, &any, &any, &any);
                const err_t e = dhcp_start(&_netif);
                if (e != ERR_OK) bmdsErr("%s: DHCP not started (lwIP error %d)", name(), e);
        }
        return;
}

void NetInterface::statusChanged(struct netif *netif) {
        auto &n = *static_cast<NetInterface *>(netif->state);
        if (!n._applying) n.logAddress();
        NetStatus::noteChange();
        // A lease lost takes its servers with it.
        NtpClient::addressChanged();
        if (ip4_addr_isany_val(*netif_ip4_addr(netif)) || n._mode != Mode::Dhcp) {
                if (n._leaseServers.ntp[0] != 0) NtpClient::serversChanged();
                n._leaseServers = LeaseServers{};
                NetNames::serversChanged();
        }
        return;
}

void NetInterface::dhcpOption(unsigned int type, unsigned int option, unsigned int length, const struct pbuf *p,
                              unsigned int offset) {
        constexpr unsigned int Ack = 5;
        constexpr unsigned int Dns = 6;
        constexpr unsigned int Ntp = 42;
        if (type == Ack) NetStatus::noteChange(); // a renewal: the lease's length, its servers
        if (type != Ack || (option != Dns && option != Ntp)) return;
        uint32_t    servers[2] = {};
        const u16_t n = static_cast<u16_t>(length / 4 < 2 ? length / 4 * 4 : 8);
        if (pbuf_copy_partial(p, servers, n, static_cast<u16_t>(offset)) != n) return;
        uint32_t(&kept)[2] = option == Dns ? _leaseServers.dns : _leaseServers.ntp;
        if (kept[0] == servers[0] && kept[1] == servers[1]) return;
        kept[0] = servers[0];
        kept[1] = servers[1];
        if (option == Ntp)
                NtpClient::serversChanged();
        else
                NetNames::serversChanged();
        return;
}

void NetInterface::logAddress() {
        // Without an address the mask and gateway don't count (lwIP clears them one at a time).
        const uint32_t a = fromLwip(netif_ip4_addr(&_netif));
        const uint32_t m = a != 0 ? fromLwip(netif_ip4_netmask(&_netif)) : 0;
        const uint32_t g = a != 0 ? fromLwip(netif_ip4_gw(&_netif)) : 0;
        if (a == _loggedAddress && m == _loggedMask && g == _loggedGateway) return;
        _loggedAddress = a;
        _loggedMask = m;
        _loggedGateway = g;
        if (a == 0) {
                bmdsInfo("%s: no IPv4 address", name());
                return;
        }
        char address[Ipv4::TextBytes + 1];
        char gateway[Ipv4::TextBytes + 1];
        Ipv4::format(a, address);
        Ipv4::format(g, gateway);
        bmdsInfo("%s: %s/%d, gateway %s (%s)", name(), address, Ipv4::prefixOf(m), g != 0 ? gateway : "none",
                 _mode == Mode::Dhcp ? "DHCP" : "static");
        return;
}

err_t NetInterface::igmpFilter(struct netif *netif, const ip4_addr_t *group, enum netif_mac_filter_action action) {
        auto          &n = *static_cast<NetInterface *>(netif->state);
        const uint32_t g = fromLwip(group);
        // RFC 1112: 01:00:5e and the group's low 23 bits.
        const MacAddress mac = {{0x01, 0x00, 0x5e, static_cast<uint8_t>((g >> 16) & 0x7f), static_cast<uint8_t>(g >> 8),
                                 static_cast<uint8_t>(g)}};
        const Error      err = action == NETIF_ADD_MAC_FILTER ? n._groups.add(mac) : n._groups.remove(mac);
        if (err.isError()) return ERR_MEM;
        return n._device.setMulticast(n._groups.groups()).isOk() ? ERR_OK : ERR_IF;
}

BMDS_NAMESPACE_END
