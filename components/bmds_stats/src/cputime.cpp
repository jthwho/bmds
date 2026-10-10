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
#include <esp_freertos_hooks.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/config.h>
#include <bmds/cputime.h>
#include <bmds/log.h>
#include <bmds/sysconf.h>

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
                const esp_err_t e = esp_register_freertos_idle_hook_for_cpu(idleHook, UBaseType_t(core));
                if (e != ESP_OK) return Error::fromEspErr(e);
        }
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
// ESP timer while counting, else 0, so no task is charged (CMakeLists.txt wraps the port's). It is
// also read by a task asking for every task's state; either way the core's idle task isn't
// running then, so its stretch is over.
extern "C" IRAM_ATTR configRUN_TIME_COUNTER_TYPE __wrap_xPortGetRunTimeCounterValue(void) {
        if (!bmds::counting) return 0;
        bmds::idleSince[esp_cpu_get_core_id()] = 0;
        return configRUN_TIME_COUNTER_TYPE(esp_timer_get_time());
}
#endif
