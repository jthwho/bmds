/**
 * @file      netstats.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network's counters in the system's registry (devplan/network.md §21, sysmon.md §6):
 * net.ethN for each interface (its device's and the interface's counts), net.ip for the
 * stack (lwIP's heap and pools, and its MIB-II counts of IP, TCP, UDP and ICMP).
 */

#include <cstdio>
#include <lwip/memp.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/stats.h>
#include <bmds/netstack.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>

#include "netinterface.h"
#include "netstats.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Unit = Stat::Unit;
        using Kind = Stat::Kind;
        using Rule = Stat::Rule;
        using Health = Stat::Health;

        // An interface's counters, net.ethN: names are an interface once shipped.
        constexpr Stat::Desc InterfaceStats[] = {
                {"link", Unit::Count, Kind::State, 0, {}, "down|up", "The link (cable in, the PHY linked)"},
                {"speed_mbps", Unit::Count, Kind::Info, 0, {}, "", "The link's speed in Mbit/s (0: no link)"},
                {"rx_bytes", Unit::Bytes, Kind::Total, Stat::KeepHistory, {}, "", "Bytes received"},
                {"tx_bytes", Unit::Bytes, Kind::Total, Stat::KeepHistory, {}, "", "Bytes sent"},
                {"rx_frames", Unit::Count, Kind::Total, 0, {}, "", "Frames received"},
                {"tx_frames", Unit::Count, Kind::Total, 0, {}, "", "Frames sent"},
                {"rx_errors",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Frames received damaged (a cable, a duplex mismatch)"},
                {"tx_errors",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Frames that failed to go"},
                {"rx_dropped",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Frames lost: missed by the MAC, its FIFO overflowed, or no memory in the stack"},
                {"tx_dropped",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Frames the stack sent that the device didn't take"},
                {"rx_copied", Unit::Count, Kind::Total, 0, {}, "", "Frames copied because the stack held many buffers"},
                {"rx_held", Unit::Count, Kind::Level, 0, {}, "", "Receive buffers the stack holds now"},
                {"link_changes", Unit::Count, Kind::Total, 0, {}, "", "Times the link went up or down"},
                {"rx_no_buffer",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Times the receive ring ran dry (a frame waits in the FIFO; those lost count as rx_overflow)"},
                {"rx_missed", Unit::Count, Kind::Total, 0, {}, "", "Frames the MAC missed with no descriptor"},
                {"rx_overflow",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Frames lost to the MAC's receive FIFO overflowing"},
                {"rx_bulk_dropped",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Frames dropped as bulk traffic held its share of buffers (real-time flows running)"},
                {"rx_checksummed", Unit::Count, Kind::Total, 0, {}, "", "Frames whose checksums the MAC checked"},
                {"tx_in_place", Unit::Count, Kind::Total, 0, {}, "", "Frames the DMA sent from the stack's buffers"},
                {"tx_realtime", Unit::Count, Kind::Total, 0, {}, "", "Real-time frames sent"},
                {"tx_realtime_waited",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Real-time frames that found the transmit ring full"},
                {"rx_restarts",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Times the MAC's receive path wedged and was reset"},
        };

        constexpr size_t InterfaceCount = sizeof(InterfaceStats) / sizeof(InterfaceStats[0]);

        // The stack's, net.ip.
        constexpr int64_t    HeapBytes = MEM_SIZE;
        constexpr Stat::Desc IpStats[] = {
                {"heap_used", Unit::Bytes, Kind::Level, Stat::KeepHistory, {}, "", "lwIP's heap in use"},
                {"heap_max",
                 Unit::Bytes,
                 Kind::Peak,
                 0,
                 {Rule::Above, Health::Warning, 0, HeapBytes * 3 / 4, HeapBytes * 15 / 16},
                 "",
                 "The most of lwIP's heap in use at once"},
                {"heap_failed",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Allocations lwIP's heap refused"},
                {"pools_failed",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Allocations lwIP's pools refused (connections, segments, timers)"},
                {"tcp_connections",
                 Unit::Count,
                 Kind::Level,
                 Stat::KeepHistory,
                 {},
                 "",
                 "TCP connections lwIP has open (opening, open, or closing)"},
                {"ip_in", Unit::Count, Kind::Total, 0, {}, "", "IPv4 packets received"},
                {"ip_out", Unit::Count, Kind::Total, 0, {}, "", "IPv4 packets sent"},
                {"ip_in_discards",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "IPv4 packets received and dropped: bad headers, not ours, unknown protocols"},
                {"tcp_active_opens", Unit::Count, Kind::Total, 0, {}, "", "TCP connections this end opened"},
                {"tcp_passive_opens", Unit::Count, Kind::Total, 0, {}, "", "TCP connections accepted"},
                {"tcp_attempt_fails", Unit::Count, Kind::Total, 0, {}, "", "TCP connections closed before they opened"},
                {"tcp_in_segs", Unit::Count, Kind::Total, 0, {}, "", "TCP segments received"},
                {"tcp_out_segs", Unit::Count, Kind::Total, 0, {}, "", "TCP segments sent"},
                {"tcp_fast_retransmits",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "TCP segments sent again on duplicate acknowledgements (not those after a timeout)"},
                {"tcp_in_errors", Unit::Count, Kind::Total, 0, {}, "", "TCP segments received damaged"},
                {"udp_in", Unit::Count, Kind::Total, 0, {}, "", "UDP datagrams received"},
                {"udp_out", Unit::Count, Kind::Total, 0, {}, "", "UDP datagrams sent"},
                {"udp_no_port", Unit::Count, Kind::Total, 0, {}, "", "UDP datagrams to a port nobody had open"},
                {"udp_in_errors", Unit::Count, Kind::Total, 0, {}, "", "UDP datagrams received damaged"},
                {"icmp_in",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "ICMP messages lwIP took (a ping's replies go to its raw socket)"},
                {"icmp_out",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "ICMP messages lwIP sent (not a raw socket's: ping's requests)"},
        };

        constexpr size_t IpCount = sizeof(IpStats) / sizeof(IpStats[0]);

        void readInterface(void *arg, Span<int64_t> v);
        void readIp(void *arg, Span<int64_t> v);

        struct InterfaceCounters {
                        InterfaceCounters() : group(InterfaceStats, InterfaceCount, readInterface, this) {}
                        NetInterface *interface = nullptr;
                        Stats::Group  group;
        };

        struct Counters {
                        Counters() : ip(IpStats, IpCount, readIp, nullptr) {}
                        Stats::Group      ip;
                        InterfaceCounters interfaces[NetInterface::MaxInterfaces];
        };

        constinit StaticObject<Counters> counters;

        // The registry's lock is held, then the core lock: the groups are added and removed
        // without the core lock.
        void readInterface(void *arg, Span<int64_t> v) {
                NetInterface *ni = static_cast<InterfaceCounters *>(arg)->interface;
                if (ni == nullptr) return;
                NetStack::QuietLocker          lock;
                const EthernetDevice::Link     link = ni->device().link();
                const EthernetDevice::Counters c = ni->device().counters();
                const NetInterface::Counters  &k = ni->counters();
                v[0] = link.up ? 1 : 0;
                v[1] = link.up ? link.speedMbps : 0;
                v[2] = int64_t(c.rxBytes);
                v[3] = int64_t(c.txBytes);
                v[4] = int64_t(c.rxFrames);
                v[5] = int64_t(c.txFrames);
                v[6] = c.rxErrors;
                v[7] = c.txErrors;
                // Not rxNoBuffer: the ring ran dry, and the frame waited in the FIFO (§30b).
                v[8] = int64_t(c.rxMissed) + c.rxOverflow + k.rxDropped;
                v[9] = k.txDropped;
                v[10] = k.rxCopied;
                v[11] = ni->held();
                v[12] = c.linkChanges;
                v[13] = c.rxNoBuffer;
                v[14] = c.rxMissed;
                v[15] = c.rxOverflow;
                v[16] = c.rxBulkDropped;
                v[17] = int64_t(c.rxChecksummed);
                v[18] = int64_t(c.txInPlace);
                v[19] = int64_t(c.txRealTime);
                v[20] = c.txRealTimeWaited;
                v[21] = c.rxRestarts;
                return;
        }

        void readIp(void *, Span<int64_t> v) {
                NetStack::QuietLocker    lock;
                const struct stats_mib2 &m = lwip_stats.mib2;
                int64_t                  poolsFailed = 0;
                for (size_t i = 0; i < MEMP_MAX; ++i) poolsFailed += lwip_stats.memp[i]->err;
                int64_t connections = 0;
                for (const struct tcp_pcb *p = tcp_active_pcbs; p != nullptr; p = p->next) ++connections;
                v[0] = int64_t(lwip_stats.mem.used);
                v[1] = int64_t(lwip_stats.mem.max);
                v[2] = lwip_stats.mem.err;
                v[3] = poolsFailed;
                v[4] = connections;
                v[5] = m.ipinreceives;
                v[6] = m.ipoutrequests;
                v[7] = int64_t(m.ipinhdrerrors) + m.ipinaddrerrors + m.ipinunknownprotos + m.ipindiscards;
                v[8] = m.tcpactiveopens;
                v[9] = m.tcppassiveopens;
                v[10] = m.tcpattemptfails;
                v[11] = m.tcpinsegs;
                v[12] = m.tcpoutsegs;
                v[13] = m.tcpretranssegs;
                v[14] = m.tcpinerrs;
                v[15] = m.udpindatagrams;
                v[16] = m.udpoutdatagrams;
                v[17] = m.udpnoports;
                v[18] = m.udpinerrors;
                v[19] = m.icmpinmsgs;
                v[20] = m.icmpoutmsgs;
                return;
        }

} // namespace

void NetStats::init() {
        Counters &c = counters.create();
        c.ip.setName("net.ip");
        (void)Stats::add(c.ip);
        for (size_t i = 0; i < NetInterface::count(); ++i) {
                InterfaceCounters &ic = c.interfaces[i];
                NetInterface      &ni = NetInterface::at(i);
                ic.interface = &ni;
                char name[Stats::MaxGroupName + 1];
                std::snprintf(name, sizeof(name), "net.%s", ni.name());
                ic.group.setName(name);
                if (Stats::add(ic.group).isError()) ic.interface = nullptr;
        }
        return;
}

BMDS_NAMESPACE_END
