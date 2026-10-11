/**
 * @file      lwipwire.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * lwIP for the host tests (lwipwire.h): the wire, lwIP's port (the
 * firmware's is netstack.cpp) and NetStack with a network thread.
 */

#include "lwipwire.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <lwip/etharp.h>
#include <lwip/init.h>
#include <lwip/stats.h>
#include <lwip/timeouts.h>
#include <netif/ethernet.h>
#include <bmds/assert.h>
#include <bmds/netstack.h>

namespace LwipWire {

        std::vector<Call> calls;
        Port              a;
        Port              b;

        namespace {

                u32_t nowMs = 0;

                // NetStack's core lock, and the network thread's wake.
                std::mutex                   core;
                std::atomic<std::thread::id> holder{};
                std::mutex                   wakeLock;
                std::condition_variable      wakeUp;
                bool                         woken = false;
                std::atomic<bool>            threaded{false};

                err_t linkOutput(struct netif *netif, struct pbuf *p) {
                        auto                &port = *static_cast<Port *>(netif->state);
                        std::vector<uint8_t> frame(p->tot_len);
                        (void)pbuf_copy_partial(p, frame.data(), p->tot_len, 0);
                        port.peer->inbox.push_back(std::move(frame));
                        return ERR_OK;
                }

                err_t igmpFilter(struct netif *netif, const ip4_addr_t *group, enum netif_mac_filter_action action) {
                        auto &port = *static_cast<Port *>(netif->state);
                        port.filterLog.push_back(std::string(action == NETIF_ADD_MAC_FILTER ? "+" : "-") +
                                                 ip4addr_ntoa(group));
                        return ERR_OK;
                }

                err_t portInit(struct netif *netif) {
                        auto &port = *static_cast<Port *>(netif->state);
                        netif->name[0] = 'e';
                        netif->name[1] = 't';
                        netif->hwaddr_len = ETH_HWADDR_LEN;
                        std::memcpy(netif->hwaddr, port.mac, 6);
                        netif->mtu = 1500;
                        netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
                        netif->output = &etharp_output;
                        netif->linkoutput = &linkOutput;
                        netif_set_igmp_mac_filter(netif, &igmpFilter);
                        return ERR_OK;
                }

                // One round: the frames on the wire into lwIP, then the calls it queued.
                void deliver() {
                        for (Port *p : {&a, &b}) {
                                while (!p->inbox.empty()) {
                                        std::vector<uint8_t> frame = std::move(p->inbox.front());
                                        p->inbox.pop_front();
                                        struct pbuf *q =
                                                pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
                                        if (q == nullptr) continue; // dropped, as a full ring would
                                        (void)pbuf_take(q, frame.data(), static_cast<u16_t>(frame.size()));
                                        if (p->netif.input(q, &p->netif) != ERR_OK) pbuf_free(q);
                                }
                        }
                        std::vector<Call> due;
                        due.swap(calls);
                        for (const Call &c : due) c.fn(c.ctx);
                        return;
                }

                std::thread       netThread;
                std::atomic<bool> stopping{false};

                void run() {
                        using Clock = std::chrono::steady_clock;
                        const auto start = Clock::now();
                        u32_t      base;
                        {
                                std::lock_guard<std::mutex> g(core);
                                base = nowMs;
                        }
                        while (!stopping.load()) {
                                u32_t sleepMs;
                                bool  more;
                                bmds::NetStack::lock();
                                deliver();
                                nowMs = base + static_cast<u32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                          Clock::now() - start)
                                                                          .count());
                                sys_check_timeouts();
                                sleepMs = sys_timeouts_sleeptime();
                                more = !a.inbox.empty() || !b.inbox.empty() || !calls.empty();
                                bmds::NetStack::unlockQuietly();
                                if (more || sleepMs == 0) continue;
                                std::unique_lock<std::mutex> w(wakeLock);
                                const auto ms = sleepMs == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? 1000u : sleepMs;
                                wakeUp.wait_for(w, std::chrono::milliseconds(ms), [] { return woken; });
                                woken = false;
                        }
                        return;
                }

        } // namespace

        ip4_addr_t addr(const char *text) {
                ip4_addr_t x;
                REQUIRE(ip4addr_aton(text, &x));
                return x;
        }

        void pump() {
                for (int round = 0; round < 8; ++round) {
                        deliver();
                        nowMs += 10;
                        sys_check_timeouts();
                }
                return;
        }

        void idle(u32_t ms) {
                for (u32_t t = 0; t < ms; t += 100) {
                        nowMs += 100;
                        sys_check_timeouts();
                }
                pump();
                return;
        }

        size_t heapUsed() {
                return lwip_stats.mem.used;
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

        NetThread::NetThread() {
                setUp();
                stopping.store(false);
                threaded.store(true);
                netThread = std::thread(&run);
        }

        NetThread::~NetThread() {
                stopping.store(true);
                bmds::NetStack::wake();
                netThread.join();
                threaded.store(false);
        }

} // namespace LwipWire

// --- NetStack, as the host tests have it (the firmware's is netstack.cpp) --------------------

namespace bmds::NetStack {

        Error init() {
                return Error::Ok;
        }

        bool isReady() {
                return true;
        }

        void lock() {
                LwipWire::core.lock();
                LwipWire::holder.store(std::this_thread::get_id());
                return;
        }

        void unlock() {
                unlockQuietly();
                wake();
                return;
        }

        void unlockQuietly() {
                LwipWire::holder.store(std::thread::id());
                LwipWire::core.unlock();
                return;
        }

        bool isLocked() {
                return LwipWire::holder.load() == std::this_thread::get_id();
        }

        void wake() {
                {
                        std::lock_guard<std::mutex> g(LwipWire::wakeLock);
                        LwipWire::woken = true;
                }
                LwipWire::wakeUp.notify_one();
                return;
        }

} // namespace bmds::NetStack

// --- lwIP's port ------------------------------------------------------------------------------

extern "C" {

unsigned char *bmds_lwip_heap = nullptr;

void bmds_lwip_assert_locked(void) {
        // Checked while the network thread runs (sockets); the hand-driven tests run alone.
        if (LwipWire::threaded.load() && !bmds::NetStack::isLocked())
                bmds_lwip_assert("lwIP without the core lock", __FILE__, __LINE__);
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
        LwipWire::calls.push_back({fn, ctx});
        return 0;
}

u32_t sys_now(void) {
        return LwipWire::nowMs;
}

struct netif *bmds_netif_named(const char *name) {
        return netif_find(name);
}

void bmds_dhcp_option(struct netif *, unsigned int, unsigned int, unsigned int, const struct pbuf *, unsigned int) {
        return;
}

int bmds_dns_local(const char *, size_t, void *) {
        return -1;
}

} // extern "C"
