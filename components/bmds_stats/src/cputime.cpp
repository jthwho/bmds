/**
 * @file      cputime.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tasks' CPU time, switched per boot (devplan/sysmon.md §3.1).
 */

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_private/esp_clk.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/config.h>
#include <bmds/cputime.h>
#include <bmds/log.h>
#include <bmds/sysconf.h>
#include <bmds/tasklist.h>

// tasks.c's: the task running on each core (at a switch, the one switched out); port.c's: each
// core's critical-section depth.
extern "C" void *volatile pxCurrentTCBs[portNUM_PROCESSORS];
extern "C" volatile UBaseType_t port_uxCriticalNesting[portNUM_PROCESSORS];

BMDS_NAMESPACE_BEGIN

namespace {

        // The key is an interface once shipped.
        BMDS_SYSCONF(sysmonEnable, "sysmon/enable", Config::Type::Bool, "true",
                     "Count each task's CPU time (applies at the next boot)", 0, 0, nullptr, nullptr)

        constexpr int Cores = portNUM_PROCESSORS;

        // Read at every context switch, from internal RAM.
        DRAM_ATTR constinit bool counting = false;

        // When each core's idle task last began running (0: it isn't). FreeRTOS adds an idle
        // stretch to the idle task's counter only when it ends, so a core idle for a long time
        // would read as busy; the idle hook marks the stretch, every switch on the core clears it.
        DRAM_ATTR constinit volatile int64_t idleSince[Cores] = {};

        // The busy time last reported, so a race with a switch never makes it go back.
        constinit int64_t lastBusy[Cores] = {};

        // The run-time clock, counted in the core's cycles between reads of the ESP timer
        // (devplan/bench.md §18). The timer costs ~240 cycles a read, at every switch; the cycle
        // counter one instruction, but it stops while its core waits for an interrupt, which only
        // its idle task does. So the timer is read when the idle task is switched out; between,
        // each read moves the whole µs counted into the base and keeps the cycles left over (no
        // drift: the CPU's clock and the timer's come from the same crystal). The tick hook does
        // the same on a core that goes on without a switch, before the 32-bit counter wraps.
        struct RunClock {
                        int64_t  baseUs;     // the time at baseCycles
                        uint32_t baseCycles; // the counter then
                        int64_t  lastUs;     // what was returned: never less after
        };
        DRAM_ATTR constinit RunClock runClock[Cores] = {};
        DRAM_ATTR constinit uint32_t cyclesPerUs = 0;
        DRAM_ATTR constinit void    *idleTask[Cores] = {};

        // Moves @p clock's base on to now (@p cycles): the time then.
        FORCE_INLINE_ATTR int64_t advance(RunClock &clock, uint32_t cycles, uint32_t perUs) {
                const uint32_t us = (cycles - clock.baseCycles) / perUs;
                clock.baseUs += us;
                clock.baseCycles += us * perUs;
                return clock.baseUs;
        }

        // Every tick on each core, in its tick interrupt (switches on the core can't run
        // meanwhile): a base more than a second old moved on (a core waiting in its idle task is
        // set right when the idle task is switched out instead).
        IRAM_ATTR void tickHook() {
                const uint32_t perUs = __atomic_load_n(&cyclesPerUs, __ATOMIC_ACQUIRE);
                const int      core = int(esp_cpu_get_core_id());
                RunClock      &clock = runClock[core];
                const uint32_t cycles = esp_cpu_get_cycle_count();
                if (perUs != 0 && cycles - clock.baseCycles > perUs * 1000000u && pxCurrentTCBs[core] != idleTask[core])
                        (void)advance(clock, cycles, perUs);
                return;
        }

        IRAM_ATTR bool idleHook() {
                const int core = int(esp_cpu_get_core_id());
                if (idleSince[core] == 0) idleSince[core] = esp_timer_get_time();
                return true; // the core may wait for an interrupt
        }

} // namespace

Error CpuTime::init() {
        auto [on, err] = SysConf::getBool(bmdsConf_sysmonEnable.key);
        counting = err.isError() || on;
        if (!isEnabled()) {
                bmdsInfo("CPU time isn't counted this boot (sysmon/enable)");
                return Error::Ok;
        }
        for (int core = 0; core < Cores; ++core) {
                idleTask[core] = xTaskGetIdleTaskHandleForCore(BaseType_t(core));
                esp_err_t e = esp_register_freertos_idle_hook_for_cpu(idleHook, UBaseType_t(core));
                if (e == ESP_OK) e = esp_register_freertos_tick_hook_for_cpu(tickHook, UBaseType_t(core));
                if (e != ESP_OK) return Error::fromEspErr(e);
        }
        // Last: the switches start counting in cycles once this is set (the timer before).
        __atomic_store_n(&cyclesPerUs, uint32_t(esp_clk_cpu_freq() / 1000000), __ATOMIC_RELEASE);
        return Error::Ok;
}

bool CpuTime::isEnabled() {
        return configGENERATE_RUN_TIME_STATS != 0 && counting;
}

int CpuTime::cores() {
        return Cores;
}

int64_t CpuTime::busyUs(int core) {
        if (!isEnabled() || core < 0 || core >= Cores) return -1;
#if configGENERATE_RUN_TIME_STATS
        const int64_t since = idleSince[core];
        const int64_t idle = int64_t(ulTaskGetIdleRunTimeCounterForCore(BaseType_t(core)));
        const int64_t now = esp_timer_get_time();
        int64_t       busy = now - idle - (since != 0 && since < now ? now - since : 0);
        if (busy < lastBusy[core]) busy = lastBusy[core];
        lastBusy[core] = busy;
        return busy;
#else
        return -1;
#endif
}

BMDS_NAMESPACE_END

#if configGENERATE_RUN_TIME_STATS
// FreeRTOS's run-time clock (portGET_RUN_TIME_COUNTER_VALUE), read at every context switch: the
// ESP timer's time, counted in cycles between its reads (RunClock), while counting, else 0, so no
// task is charged (CMakeLists.txt wraps the port's). At a switch the core's idle task's stretch,
// if it was running, is over.


extern "C" IRAM_ATTR configRUN_TIME_COUNTER_TYPE __wrap_xPortGetRunTimeCounterValue(void) {
        if (!bmds::counting) return 0;
        // At a switch the kernel's lock is held, interrupts masked: nothing else on this core
        // touches its clock. Any other caller (none in BMDS) gets the timer, the clock untouched.
        const int core = esp_cpu_get_core_id();
        if (port_uxCriticalNesting[core] == 0) return configRUN_TIME_COUNTER_TYPE(esp_timer_get_time());
        bmds::RunClock &clock = bmds::runClock[core];
        const uint32_t  cycles = esp_cpu_get_cycle_count();
        const uint32_t  perUs = __atomic_load_n(&bmds::cyclesPerUs, __ATOMIC_ACQUIRE);
        int64_t         now;
        if (perUs == 0 || pxCurrentTCBs[core] == bmds::idleTask[core]) {
                now = esp_timer_get_time();
                clock.baseUs = now;
                clock.baseCycles = cycles;
        } else {
                now = bmds::advance(clock, cycles, perUs);
        }
        if (now < clock.lastUs) now = clock.lastUs;
        clock.lastUs = now;
        bmds::idleSince[core] = 0;
        bmds::TaskList::switchedInUs[core] = now; // where the task switched in now starts counting
        return configRUN_TIME_COUNTER_TYPE(now);
}
#endif
