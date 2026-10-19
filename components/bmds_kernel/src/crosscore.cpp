/**
 * @file      crosscore.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ESP-IDF's cross-core interrupt (esp_system/crosscore_int.c), cheaper (devplan/bench.md §18).
 * Every yield a task makes, to its own core or the other, is this interrupt: the port triggers it
 * and switches tasks on the way out of it. IDF keeps each core's reasons behind a spinlock, taken
 * by the sender and again by the handler: two critical sections a switch, ~360 cycles of a
 * same-core switch's ~3,400. Here the reasons are an atomic word (amoor to send, amoswap to
 * take). These replace esp_crosscore_int_init() and the senders (-Wl,--wrap, CMakeLists.txt), so
 * IDF's handler is never installed; the interrupt and its register are IDF's own, which
 * vPortYield() reads while it waits for the switch.
 */

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_debug_helpers.h>
#include <esp_intr_alloc.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <hal/crosscore_int_ll.h>
#include <soc/system_intr.h>

#if CONFIG_ESP_TASK_WDT_EN
extern "C" void task_wdt_timeout_abort(bool current_core); // task_wdt.c's (not in a header)
#endif

namespace {

        constexpr uint32_t ReasonYield = 1u << 0;
        constexpr uint32_t ReasonFreqSwitch = 1u << 1; // handled by the port before the handler: nothing here
        constexpr uint32_t ReasonPrintBacktrace = 1u << 2;
        constexpr uint32_t ReasonGdbCall = 1u << 3; // no runtime GDB stub in BMDS: nothing here
        constexpr uint32_t ReasonTwdtAbort = 1u << 4;

        DRAM_ATTR constinit uint32_t reasons[portNUM_PROCESSORS] = {};

        IRAM_ATTR void handler(void *arg) {
                const int core = int(esp_cpu_get_core_id());
                // Cleared before the reasons are taken: a reason sent after the take raises it again.
                crosscore_int_ll_clear_interrupt(core);
                __asm__ volatile("fence" ::: "memory");
                const uint32_t why = __atomic_exchange_n(static_cast<uint32_t *>(arg), 0u, __ATOMIC_ACQ_REL);
                if ((why & ReasonYield) != 0) portYIELD_FROM_ISR();
                if ((why & ReasonPrintBacktrace) != 0) esp_backtrace_print(100);
#if CONFIG_ESP_TASK_WDT_EN
                if ((why & ReasonTwdtAbort) != 0) task_wdt_timeout_abort(false); // the other core's watchdog fired
#endif
                return;
        }

        // Sets @p why for @p core, then raises its interrupt (the reason visible first).
        IRAM_ATTR void send(int core, uint32_t why) {
                __atomic_fetch_or(&reasons[core], why, __ATOMIC_ACQ_REL);
                __asm__ volatile("fence" ::: "memory");
                crosscore_int_ll_trigger_interrupt(core);
                return;
        }

} // namespace

extern "C" void __wrap_esp_crosscore_int_init(void) {
        const int core = int(esp_cpu_get_core_id());
        __atomic_store_n(&reasons[core], 0u, __ATOMIC_RELEASE);
        int flags = 0;
#if CONFIG_ESP_SYSTEM_IN_IRAM
        flags |= ESP_INTR_FLAG_IRAM;
#endif
        const int source = core == 0 ? SYS_CPU_INTR_FROM_CPU_0_SOURCE : SYS_CPU_INTR_FROM_CPU_1_SOURCE;
        ESP_ERROR_CHECK(esp_intr_alloc(source, flags, handler, &reasons[core], nullptr));
        return;
}

extern "C" IRAM_ATTR void __wrap_esp_crosscore_int_send_yield(int core) {
        send(core, ReasonYield);
        return;
}

extern "C" IRAM_ATTR void __wrap_esp_crosscore_int_send_freq_switch(int core) {
        send(core, ReasonFreqSwitch);
        return;
}

extern "C" IRAM_ATTR void __wrap_esp_crosscore_int_send_gdb_call(int core) {
        send(core, ReasonGdbCall);
        return;
}

extern "C" IRAM_ATTR void __wrap_esp_crosscore_int_send_print_backtrace(int core) {
        send(core, ReasonPrintBacktrace);
        return;
}

extern "C" IRAM_ATTR void __wrap_esp_crosscore_int_send_twdt_abort(int core) {
        send(core, ReasonTwdtAbort);
        return;
}
