/**
 * @file      netstatus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network interfaces' state (netstatus.h), read from lwIP's netifs,
 * the interfaces and their devices under the core lock.
 */

#include <cstring>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <bmds/netstack.h>
#include <bmds/netstatus.h>

#include "netinterface.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The netif at @p position in index order, or null. Core lock held.
        struct netif *netifAt(size_t position) {
                size_t seen = 0;
                for (unsigned int index = 1; index <= 255; ++index) {
                        struct netif *n = netif_get_by_index(static_cast<u8_t>(index));
                        if (n == nullptr) continue;
                        if (seen++ == position) return n;
                }
                return nullptr;
        }

        NetInterface *interfaceOf(struct netif *n) {
                for (size_t i = 0; i < NetInterface::count(); ++i) {
                        if (&NetInterface::at(i).netif() == n) return &NetInterface::at(i);
                }
                return nullptr;
        }

} // namespace

size_t NetStatus::count() {
        if (!NetStack::isReady()) return 0;
        NetStack::QuietLocker lock;
        size_t                n = 0;
        struct netif         *i;
        NETIF_FOREACH(i) {
                (void)i;
                ++n;
        }
        return n;
}

Error NetStatus::info(size_t position, Info &info) {
        info = Info{};
        if (!NetStack::isReady()) return Error::NotExist;
        NetStack::QuietLocker lock;
        struct netif         *n = netifAt(position);
        if (n == nullptr) return Error::NotExist;
        NetInterface *ni = interfaceOf(n);
        const char   *name = ni != nullptr ? ni->name() : "lo";
        std::strncpy(info.name, name, sizeof(info.name) - 1);
        info.index = netif_get_index(n);
        info.up = netif_is_up(n);
        info.loopback = ni == nullptr;
        info.broadcast = (n->flags & NETIF_FLAG_BROADCAST) != 0;
        info.multicast = (n->flags & NETIF_FLAG_IGMP) != 0;
        info.mtu = n->mtu;
        info.address = ip4_addr_get_u32(netif_ip4_addr(n));
        info.netmask = ip4_addr_get_u32(netif_ip4_netmask(n));
        info.gateway = ip4_addr_get_u32(netif_ip4_gw(n));
        if (ni == nullptr) {
                info.link = netif_is_link_up(n);
                info.mode = Mode::Static;
                return Error::Ok;
        }
        std::memcpy(info.mac, n->hwaddr, sizeof(info.mac));
        const EthernetDevice::Link link = ni->device().link();
        info.link = link.up;
        info.fullDuplex = link.fullDuplex;
        info.speedMbps = link.speedMbps;
        switch (ni->mode()) {
                case NetInterface::Mode::Dhcp: info.mode = Mode::Dhcp; break;
                case NetInterface::Mode::Static: info.mode = Mode::Static; break;
                case NetInterface::Mode::Off: info.mode = Mode::Off; break;
        }
        const struct dhcp *d = netif_dhcp_data(n);
        if (info.mode == Mode::Dhcp && d != nullptr) {
                info.dhcp = dhcp_supplied_address(n) ? Dhcp::Bound : Dhcp::Asking;
                if (info.dhcp == Dhcp::Bound) {
                        info.dhcpServer = ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr));
                        info.leaseSeconds = d->offered_t0_lease;
                        const uint32_t used = static_cast<uint32_t>(d->lease_used) * DHCP_COARSE_TIMER_SECS;
                        info.leaseLeftSeconds = used < info.leaseSeconds ? info.leaseSeconds - used : 0;
                }
        }
        const EthernetDevice::Counters c = ni->device().counters();
        const NetInterface::Counters  &k = ni->counters();
        info.rxBytes = c.rxBytes;
        info.txBytes = c.txBytes;
        info.rxFrames = static_cast<uint32_t>(c.rxFrames);
        info.txFrames = static_cast<uint32_t>(c.txFrames);
        info.rxErrors = c.rxErrors;
        info.txErrors = c.txErrors;
        info.rxDropped = c.rxNoBuffer + c.rxMissed + c.rxOverflow + k.rxDropped;
        info.txDropped = k.txDropped;
        return Error::Ok;
}

BMDS_NAMESPACE_END
