/**
 * @file      lwip.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * lwIP as BMDS configures it (devplan/network.md §15: the firmware's
 * lwipopts.h), on two Ethernet interfaces wired back to back: ARP and ping,
 * UDP, IGMP's requests for MAC filters and IP's own drop of groups not
 * joined (what passing all multicast relies on), loopback through the
 * scheduled poll, and lwIP's heap back where it was after each. Also the
 * multicast filter's reference counts.
 */

#include <doctest.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <lwip/etharp.h>
#include <lwip/igmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/prot/icmp.h>
#include <lwip/raw.h>
#include <lwip/stats.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <netif/ethernet.h>
#include <bmds/assert.h>
#include <bmds/multicastfilter.h>

#include "lwipport.h"

namespace {

        u32_t nowMs = 0;

        struct Call {
                        tcpip_callback_fn fn;
                        void             *ctx;
        };
        std::vector<Call> calls;

        // One side of the wire: its interface, the frames sent to it, the filter requests.
        struct Port {
                        struct netif                     netif = {};
                        uint8_t                          mac[6] = {};
                        Port                            *peer = nullptr;
                        std::deque<std::vector<uint8_t>> inbox;
                        std::vector<std::string>         filterLog;
        };

        Port a;
        Port b;

        err_t linkOutput(struct netif *netif, struct pbuf *p) {
                auto                &port = *static_cast<Port *>(netif->state);
                std::vector<uint8_t> frame(p->tot_len);
                (void)pbuf_copy_partial(p, frame.data(), p->tot_len, 0);
                port.peer->inbox.push_back(std::move(frame));
                return ERR_OK;
        }

        err_t igmpFilter(struct netif *netif, const ip4_addr_t *group, enum netif_mac_filter_action action) {
                auto &port = *static_cast<Port *>(netif->state);
                port.filterLog.push_back(std::string(action == NETIF_ADD_MAC_FILTER ? "+" : "-") + ip4addr_ntoa(group));
                return ERR_OK;
        }

        err_t portInit(struct netif *netif) {
                auto &port = *static_cast<Port *>(netif->state);
                netif->hwaddr_len = ETH_HWADDR_LEN;
                std::memcpy(netif->hwaddr, port.mac, 6);
                netif->mtu = 1500;
                netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
                netif->output = &etharp_output;
                netif->linkoutput = &linkOutput;
                netif_set_igmp_mac_filter(netif, &igmpFilter);
                return ERR_OK;
        }

        ip4_addr_t addr(const char *text) {
                ip4_addr_t x;
                REQUIRE(ip4addr_aton(text, &x));
                return x;
        }

        // Delivers what is on the wire, runs the calls lwIP queued and its timers, a few times
        // over (replies make more frames), 10 ms of lwIP's time each.
        void pump() {
                for (int round = 0; round < 8; ++round) {
                        for (Port *p : {&a, &b}) {
                                while (!p->inbox.empty()) {
                                        std::vector<uint8_t> frame = std::move(p->inbox.front());
                                        p->inbox.pop_front();
                                        struct pbuf *q =
                                                pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
                                        REQUIRE(q != nullptr);
                                        (void)pbuf_take(q, frame.data(), static_cast<u16_t>(frame.size()));
                                        if (p->netif.input(q, &p->netif) != ERR_OK) pbuf_free(q);
                                }
                        }
                        std::vector<Call> due;
                        due.swap(calls);
                        for (const Call &c : due) c.fn(c.ctx);
                        nowMs += 10;
                        sys_check_timeouts();
                }
                return;
        }

        // Lets @p ms of lwIP's time pass (its timers: ARP giving up, IGMP's reports).
        void idle(u32_t ms) {
                for (u32_t t = 0; t < ms; t += 100) {
                        nowMs += 100;
                        sys_check_timeouts();
                }
                pump();
                return;
        }

        void setUp() {
                static bool done = false;
                if (done) return;
                done = true;
                static unsigned char heap[MEM_SIZE + 2 * MEM_ALIGNMENT] __attribute__((aligned(MEM_ALIGNMENT)));
                bmds_lwip_heap = heap;
                lwip_init();
                const uint8_t macA[6] = {0x02, 0, 0, 0, 0, 0x0a};
                const uint8_t macB[6] = {0x02, 0, 0, 0, 0, 0x0b};
                std::memcpy(a.mac, macA, 6);
                std::memcpy(b.mac, macB, 6);
                a.peer = &b;
                b.peer = &a;
                const ip4_addr_t mask = addr("255.255.255.0");
                const ip4_addr_t ipA = addr("10.0.0.1");
                const ip4_addr_t ipB = addr("10.0.0.2");
                const ip4_addr_t none = addr("0.0.0.0");
                REQUIRE(netif_add(&a.netif, &ipA, &mask, &none, &a, &portInit, &ethernet_input) != nullptr);
                REQUIRE(netif_add(&b.netif, &ipB, &mask, &none, &b, &portInit, &ethernet_input) != nullptr);
                for (Port *p : {&a, &b}) {
                        netif_set_up(&p->netif);
                        netif_set_link_up(&p->netif);
                }
                pump();
                return;
        }

        // An ICMP echo, its replies counted by a raw PCB.
        struct Echo {
                        int replies = 0;

                        static u8_t receive(void *arg, struct raw_pcb *, struct pbuf *p, const ip_addr_t *) {
                                uint8_t     icmp[8];
                                const u16_t ihl =
                                        static_cast<u16_t>((static_cast<uint8_t *>(p->payload)[0] & 0x0f) * 4);
                                if (pbuf_copy_partial(p, icmp, 8, ihl) != 8 || icmp[0] != ICMP_ER) return 0;
                                ++static_cast<Echo *>(arg)->replies;
                                pbuf_free(p);
                                return 1;
                        }
        };

        int ping(struct netif *from, const char *to) {
                Echo            echo;
                struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
                REQUIRE(pcb != nullptr);
                raw_recv(pcb, &Echo::receive, &echo);
                if (from != nullptr) raw_bind_netif(pcb, from);
                struct pbuf *p = pbuf_alloc(PBUF_IP, 8 + 32, PBUF_RAM);
                REQUIRE(p != nullptr);
                auto *m = static_cast<uint8_t *>(p->payload);
                std::memset(m, 0, 8 + 32);
                m[0] = ICMP_ECHO;
                m[5] = 7; // id
                m[7] = 1; // sequence
                const u16_t sum = inet_chksum(m, 8 + 32);
                std::memcpy(m + 2, &sum, 2);
                ip_addr_t dest;
                REQUIRE(ipaddr_aton(to, &dest));
                CHECK(raw_sendto(pcb, p, &dest) == ERR_OK);
                pbuf_free(p);
                pump();
                raw_remove(pcb);
                return echo.replies;
        }

        struct Inbox {
                        std::vector<std::string> datagrams;

                        static void receive(void *arg, struct udp_pcb *, struct pbuf *p, const ip_addr_t *, u16_t) {
                                std::string s(p->tot_len, '\0');
                                (void)pbuf_copy_partial(p, s.data(), p->tot_len, 0);
                                static_cast<Inbox *>(arg)->datagrams.push_back(s);
                                pbuf_free(p);
                        }
        };

        void sendUdp(struct netif *from, const char *to, u16_t port, const char *text) {
                struct udp_pcb *pcb = udp_new();
                REQUIRE(pcb != nullptr);
                udp_bind_netif(pcb, from);
                const u16_t  n = static_cast<u16_t>(std::strlen(text));
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
                REQUIRE(p != nullptr);
                (void)pbuf_take(p, text, n);
                ip_addr_t dest;
                REQUIRE(ipaddr_aton(to, &dest));
                CHECK(udp_sendto(pcb, p, &dest, port) == ERR_OK);
                pbuf_free(p);
                udp_remove(pcb);
                pump();
                return;
        }

} // namespace

// --- lwIP's port, as the host tests have it (the firmware's is netstack.cpp) -----------------

extern "C" {

unsigned char *bmds_lwip_heap = nullptr;

void bmds_lwip_assert_locked(void) {
        return;
}

unsigned int bmds_lwip_rand(void) {
        static unsigned int x = 12345;
        x = x * 1103515245u + 12345u;
        return x;
}

void bmds_lwip_assert(const char *message, const char *file, int line) {
        bmds::Detail::assertFailed(message, file, line);
}

int bmds_lwip_schedule(tcpip_callback_fn fn, void *ctx) {
        calls.push_back({fn, ctx});
        return 0;
}

u32_t sys_now(void) {
        return nowMs;
}

} // extern "C"

TEST_CASE("lwIP: ARP and ping between two interfaces") {
        setUp();
        const mem_size_t used = lwip_stats.mem.used;
        CHECK(ping(&a.netif, "10.0.0.2") == 1);
        CHECK(ping(&b.netif, "10.0.0.1") == 1);
        struct eth_addr  *mac = nullptr;
        const ip4_addr_t *ip = nullptr;
        const ip4_addr_t  ipB = addr("10.0.0.2");
        REQUIRE(etharp_find_addr(&a.netif, &ipB, &mac, &ip) >= 0);
        CHECK(std::memcmp(mac->addr, b.mac, 6) == 0);
        CHECK(ping(&a.netif, "10.0.0.9") == 0); // nobody: ARP asks, and gives up (the echo freed)
        idle(10'000);
        CHECK(lwip_stats.mem.used == used);
}

TEST_CASE("lwIP: UDP between two interfaces") {
        setUp();
        const mem_size_t used = lwip_stats.mem.used;
        Inbox            inbox;
        struct udp_pcb  *pcb = udp_new();
        REQUIRE(pcb != nullptr);
        const ip4_addr_t ipB = addr("10.0.0.2");
        REQUIRE(udp_bind(pcb, &ipB, 5000) == ERR_OK);
        udp_recv(pcb, &Inbox::receive, &inbox);
        sendUdp(&a.netif, "10.0.0.2", 5000, "hello");
        sendUdp(&a.netif, "10.0.0.2", 5001, "nobody listens");
        REQUIRE(inbox.datagrams.size() == 1);
        CHECK(inbox.datagrams[0] == "hello");
        udp_remove(pcb);
        pump();
        CHECK(lwip_stats.mem.used == used);
}

TEST_CASE("lwIP: IGMP asks for MAC filters, IP drops the groups not joined") {
        setUp();
        const mem_size_t used = lwip_stats.mem.used;
        // The all-systems group from the start.
        REQUIRE_FALSE(a.filterLog.empty());
        CHECK(a.filterLog[0] == "+224.0.0.1");
        a.filterLog.clear();

        Inbox           inbox;
        struct udp_pcb *pcb = udp_new();
        REQUIRE(pcb != nullptr);
        REQUIRE(udp_bind(pcb, IP4_ADDR_ANY4, 6000) == ERR_OK);
        udp_recv(pcb, &Inbox::receive, &inbox);

        // The wire delivers everything, as the MAC does while it passes all multicast: frames for
        // a group not joined reach IP, which drops them.
        sendUdp(&b.netif, "239.1.2.3", 6000, "early");
        CHECK(inbox.datagrams.empty());

        const ip4_addr_t group = addr("239.1.2.3");
        REQUIRE(igmp_joingroup_netif(&a.netif, &group) == ERR_OK);
        REQUIRE(a.filterLog.size() == 1);
        CHECK(a.filterLog[0] == "+239.1.2.3");
        CHECK(igmp_joingroup_netif(&a.netif, &group) == ERR_OK); // a second user: no new filter
        CHECK(a.filterLog.size() == 1);
        sendUdp(&b.netif, "239.1.2.3", 6000, "joined");
        REQUIRE(inbox.datagrams.size() == 1);
        CHECK(inbox.datagrams[0] == "joined");
        CHECK(ping(&b.netif, "239.1.2.3") == 1); // LWIP_MULTICAST_PING

        REQUIRE(igmp_leavegroup_netif(&a.netif, &group) == ERR_OK);
        CHECK(a.filterLog.size() == 1); // one user left
        REQUIRE(igmp_leavegroup_netif(&a.netif, &group) == ERR_OK);
        REQUIRE(a.filterLog.size() == 2);
        CHECK(a.filterLog[1] == "-239.1.2.3");
        sendUdp(&b.netif, "239.1.2.3", 6000, "left");
        CHECK(inbox.datagrams.size() == 1);
        CHECK(igmp_leavegroup_netif(&a.netif, &group) == ERR_VAL);

        udp_remove(pcb);
        pump();
        CHECK(lwip_stats.mem.used == used);
}

TEST_CASE("lwIP: loopback through the scheduled poll") {
        setUp();
        const mem_size_t used = lwip_stats.mem.used;
        CHECK(ping(nullptr, "127.0.0.1") == 1);
        CHECK(ping(nullptr, "10.0.0.1") == 1); // an interface's own address loops back too
        CHECK(calls.empty());
        CHECK(lwip_stats.mem.used == used);
}

TEST_CASE("MulticastFilter: users counted per address, order kept") {
        using bmds::MacAddress;
        bmds::MulticastFilter f;
        const MacAddress      g1 = {{0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}};
        const MacAddress      g2 = {{0x01, 0x00, 0x5e, 0x01, 0x02, 0x03}};
        const MacAddress      unicast = {{0x02, 0, 0, 0, 0, 1}};
        CHECK(f.add(unicast) == bmds::Error::Invalid);
        CHECK(f.add(g1).isOk());
        CHECK(f.add(g2).isOk());
        CHECK(f.add(g1).isOk()); // 224.0.0.1 and 224.128.0.1 share an address
        REQUIRE(f.groups().size() == 2);
        CHECK(f.groups()[0] == g1);
        CHECK(f.users(g1) == 2);
        CHECK(f.remove(g1).isOk());
        CHECK(f.groups().size() == 2);
        CHECK(f.remove(g1).isOk());
        REQUIRE(f.groups().size() == 1);
        CHECK(f.groups()[0] == g2);
        CHECK(f.users(g1) == 0);
        CHECK(f.remove(g1) == bmds::Error::NotExist);
        for (size_t i = 1; i < bmds::MulticastFilter::MaxGroups; ++i) {
                MacAddress g = g2;
                g.bytes[5] = static_cast<uint8_t>(0x80 + i);
                CHECK(f.add(g).isOk());
        }
        MacAddress more = g2;
        more.bytes[4] = 0x7f;
        CHECK(f.add(more) == bmds::Error::Full);
        CHECK(f.groups().size() == bmds::MulticastFilter::MaxGroups);
}
