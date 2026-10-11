/**
 * @file      netstack.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The IP stack (devplan/network.md §15): lwIP's heap, its task, its lock,
 * and what lwIP's port asks of BMDS (lwipport.h).
 */

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <bmds/assert.h>
#include <bmds/ethernetdevice.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/netstack.h>
#include <bmds/random.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>
#include <bmds/staticobject.h>
#include <bmds/system.h>
#include <bmds/task.h>

#include "lwipport.h"
#include "netchanges.h"
#include "netinterface.h"
#include "netmdns.h"
#include "netnames.h"
#include "netstats.h"
#include "ntpclient.h"

extern "C" {
unsigned char *bmds_lwip_heap = nullptr;
}

BMDS_NAMESPACE_BEGIN

namespace {

        // Calls queued for the task (lwIP's loopback): a few at a time is plenty.
        constexpr size_t MaxCalls = 8;

        struct Call {
                        tcpip_callback_fn fn = nullptr;
                        void             *ctx = nullptr;
        };

        struct Stack {
                        Mutex                     core;
                        std::atomic<TaskHandle_t> holder{nullptr};
                        Semaphore                 wake{1, 0};
                        Task                      task;
                        SpinLock                  callLock;
                        Call                      calls[MaxCalls];
                        size_t                    callCount = 0;
        };

        constinit StaticObject<Stack> stack;
        constinit std::atomic<bool>   ready{false};

        // Runs what was queued (core lock held), oldest first.
        void runCalls() {
                for (;;) {
                        Call c;
                        {
                                SpinLock::Locker lock(stack->callLock);
                                if (stack->callCount == 0) return;
                                c = stack->calls[0];
                                for (size_t i = 1; i < stack->callCount; ++i) stack->calls[i - 1] = stack->calls[i];
                                --stack->callCount;
                        }
                        c.fn(c.ctx);
                }
        }

        void run(void *) {
                for (;;) {
                        uint32_t sleepMs;
                        {
                                NetStack::Locker lock;
                                NetInterface::pollAll();
                                NetNames::poll();
                                NtpClient::poll();
                                NetMdns::poll();
                                runCalls();
                                sys_check_timeouts();
                                NetStatus::publish(); // after all of it: one change told for several
                                sleepMs = sys_timeouts_sleeptime();
                        }
                        if (sleepMs == 0) continue;
                        (void)stack->wake.acquire(sleepMs == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? WaitForever : sleepMs);
                }
        }

} // namespace

Error NetStack::init() {
        // lwIP's heap, and the pools carved from it, in PSRAM (lwipopts.h).
        bmds_lwip_heap = static_cast<unsigned char *>(
                Memory::allocAligned(MemSpace::Psram, MEM_SIZE + 2 * MEM_ALIGNMENT, MEM_ALIGNMENT));
        if (bmds_lwip_heap == nullptr) return Error::NoMem;
        stack.create();
        Error err = Error::Ok;
        {
                Locker lock;
                lwip_init();
                // lwIP leaves the loopback's MTU 0, its "no limit"; programs read it, so the
                // largest IPv4 packet instead (TCP's MSS is the smaller of it and TCP_MSS).
                if (struct netif *lo = netif_find("lo0"); lo != nullptr) lo->mtu = 0xffff;
                for (size_t i = 0; i < EthernetDevice::count() && err.isOk(); ++i) {
                        auto [device, e] = EthernetDevice::device(i);
                        err = e.isOk() ? NetInterface::create(*device) : e;
                }
        }
        if (err.isError()) return err;
        NetStats::init(); // counters only: the stack works without them
        err = NetNames::init();
        if (err.isError()) return err;
        err = NtpClient::init();
        if (err.isError()) return err;
        err = NetMdns::init();
        if (err.isError()) return err;
        Task::Config tc;
        tc.name = "net";
        tc.stackBytes = 6144;
        tc.priority = CONFIG_BMDS_NET_STACK_PRIORITY;
        err = stack->task.start(tc, &run, nullptr);
        if (err.isError()) return err;
        ready.store(true);
        return Error::Ok;
}

bool NetStack::isReady() {
        return ready.load();
}

void NetStack::lock() {
        (void)stack->core.lock(WaitForever);
        stack->holder.store(xTaskGetCurrentTaskHandle(), std::memory_order_relaxed);
        return;
}

void NetStack::unlock() {
        stack->holder.store(nullptr, std::memory_order_relaxed);
        stack->core.unlock();
        // The holder may have set a timer sooner than the task's sleep.
        if (stack->task.handle() != xTaskGetCurrentTaskHandle()) stack->wake.release();
        return;
}

void NetStack::unlockQuietly() {
        stack->holder.store(nullptr, std::memory_order_relaxed);
        stack->core.unlock();
        return;
}

bool NetStack::isLocked() {
        return stack->holder.load(std::memory_order_relaxed) == xTaskGetCurrentTaskHandle();
}

void NetStack::wake() {
        stack->wake.release();
        return;
}

BMDS_NAMESPACE_END

// --- lwIP's port (lwipport.h, lwip/sys.h) ---------------------------------------------------

extern "C" {

u32_t sys_now(void) {
        return static_cast<u32_t>(bmds::System::uptimeUs() / 1000);
}

void bmds_lwip_assert_locked(void) {
        if (!bmds::NetStack::isLocked()) bmds_lwip_assert("lwIP without the core lock", __FILE__, __LINE__);
        return;
}

unsigned int bmds_lwip_rand(void) {
        unsigned int r;
        bmds::Random::fill(&r, sizeof(r));
        return r;
}

void bmds_lwip_assert(const char *message, const char *file, int line) {
        bmds::Detail::assertFailed(message, file, line);
}

struct netif *bmds_netif_named(const char *name) {
        if (bmds::StringView(name) == "lo") return netif_find("lo0"); // lwIP's loopback
        bmds::NetInterface *n = bmds::NetInterface::find(name);
        return n != nullptr ? &n->netif() : nullptr;
}

int bmds_lwip_schedule(tcpip_callback_fn fn, void *ctx) {
        {
                bmds::SpinLock::Locker lock(bmds::stack->callLock);
                if (bmds::stack->callCount == bmds::MaxCalls) return -1;
                bmds::stack->calls[bmds::stack->callCount++] = {fn, ctx};
        }
        bmds::stack->wake.release();
        return 0;
}

} // extern "C"
