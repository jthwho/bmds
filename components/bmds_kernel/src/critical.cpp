/**
 * @file      critical.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * FreeRTOS's critical sections, cheaper (devplan/bench.md §16). Every queue, semaphore, mutex,
 * notification and task switch enters several. ESP-IDF's port masks interrupts, then takes the
 * spinlock through spinlock_acquire(), which masks them again itself and calls
 * esp_cpu_compare_and_set() in flash; leaving undoes both. On this chip's CLIC before v3 a mask is
 * a memory-mapped threshold write and read back with MIE off around it: 283 cycles a section on
 * the board, 178 with these. They replace xPortEnterCriticalTimeout() and
 * vPortExitCriticalMultiCore() (-Wl,--wrap, CMakeLists.txt) and keep their state exactly
 * (port_uxCriticalNesting, port_uxOldInterruptState, the spinlock's owner and count): a section
 * entered by port.c's own unwrapped calls can be left by ours and the other way round, and
 * spinlock_acquire() used directly on the same lock still works.
 */

#include <assert.h>
#include <freertos/FreeRTOS.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <riscv/rv_utils.h>
#include <soc/interrupt_reg.h>
#include <spinlock.h>

extern "C" {
// port.c's (not in its header): each core's nesting, the threshold its outermost entry saved, and
// how deep in interrupts it is.
extern volatile UBaseType_t port_uxCriticalNesting[portNUM_PROCESSORS];
extern volatile UBaseType_t port_uxOldInterruptState[portNUM_PROCESSORS];
extern volatile UBaseType_t port_uxInterruptNesting[portNUM_PROCESSORS];
}

namespace {

        static_assert(portNUM_PROCESSORS == 2, "the owner ids below are for two cores");

        // Raises the interrupt threshold to the critical level: the old value. MIE off around the
        // write until the read back makes it count, as IDF has it: the P4 nests interrupts in
        // hardware, so nothing saves the threshold for a task. An interrupt taken after the write
        // but before it counted could switch to another task, which would then run masked.
        FORCE_INLINE_ATTR uint32_t mask() {
#if INTTHRESH_STANDARD
                return RV_SWAP_CSR(MINTTHRESH_CSR, RVHAL_EXCM_LEVEL_CLIC);
#else
                const uint32_t status = RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
                const uint32_t old = REG_READ(CLIC_INT_THRESH_REG);
                REG_WRITE(CLIC_INT_THRESH_REG, RVHAL_EXCM_LEVEL_CLIC);
                (void)REG_READ(CLIC_INT_THRESH_REG);
                RV_SET_CSR(mstatus, status & MSTATUS_MIE);
                return old;
#endif
        }

        // Restores the threshold mask() returned. Three instructions after it, as
        // vPortClearInterruptMaskFromISR() has: a yield the unmasking lets in must happen before
        // the caller's next instruction.
        FORCE_INLINE_ATTR void unmask(uint32_t old) {
#if INTTHRESH_STANDARD
                RV_WRITE_CSR(MINTTHRESH_CSR, old);
#else
                REG_WRITE(CLIC_INT_THRESH_REG, old);
                (void)REG_READ(CLIC_INT_THRESH_REG);
#endif
                __asm__ volatile("nop\n nop\n nop" ::: "memory");
                return;
        }

        // The spinlock's compare-and-set, inline (esp_cpu_compare_and_set() is a call into flash).
        FORCE_INLINE_ATTR bool takeLock(spinlock_t *lock, uint32_t me) {
                uint32_t expected = SPINLOCK_FREE;
                return __atomic_compare_exchange_n(&lock->owner, &expected, me, false, __ATOMIC_ACQUIRE,
                                                   __ATOMIC_RELAXED);
        }

} // namespace

extern "C" IRAM_ATTR BaseType_t __wrap_xPortEnterCriticalTimeout(portMUX_TYPE *mux, BaseType_t timeout) {
        const uint32_t old = mask();
        const uint32_t core = rv_utils_get_core_id();
        const uint32_t me = core == 0 ? SPINLOCK_OWNER_ID_0 : SPINLOCK_OWNER_ID_1;
        if (mux->owner != me) { // ours already: nested, only the count
                if (!takeLock(mux, me)) {
                        if (timeout == SPINLOCK_NO_WAIT) {
                                unmask(old);
                                return pdFAIL;
                        }
                        const uint32_t start = esp_cpu_get_cycle_count();
                        while (!takeLock(mux, me)) {
                                if (timeout != SPINLOCK_WAIT_FOREVER &&
                                    esp_cpu_get_cycle_count() - start > static_cast<uint32_t>(timeout)) {
                                        unmask(old);
                                        return pdFAIL;
                                }
                        }
                }
                assert(mux->count == 0);
        }
        mux->count++;
        const UBaseType_t nesting = port_uxCriticalNesting[core] + 1;
        port_uxCriticalNesting[core] = nesting;
        if (nesting == 1) port_uxOldInterruptState[core] = old;
        return pdPASS;
}

extern "C" IRAM_ATTR void __wrap_vPortExitCriticalMultiCore(portMUX_TYPE *mux) {
        // Interrupts are masked (we hold the section): nothing here can be interrupted below it.
        const uint32_t core = rv_utils_get_core_id();
        assert(mux->owner == (core == 0 ? SPINLOCK_OWNER_ID_0 : SPINLOCK_OWNER_ID_1));
        if (--mux->count == 0) __atomic_store_n(&mux->owner, SPINLOCK_FREE, __ATOMIC_RELEASE);
        const UBaseType_t nesting = port_uxCriticalNesting[core];
        configASSERT(nesting > 0);
        if (nesting > 0) {
                port_uxCriticalNesting[core] = nesting - 1;
                if (nesting == 1) unmask(port_uxOldInterruptState[core]);
        }
        return;
}

// Whether the caller is an interrupt handler (portCHECK_IF_IN_ISR(), which every safe critical
// section asks on entering and leaving, the switch's among them). The port's masks interrupts by
// the threshold to read its core's count atomically (~64 cycles); MIE off does it in ~17: no
// interrupt, so no switch to the other core between reading the core and the count (§18).
extern "C" IRAM_ATTR BaseType_t __wrap_xPortInIsrContext(void) {
        const uint32_t   status = RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
        const BaseType_t nesting = BaseType_t(port_uxInterruptNesting[rv_utils_get_core_id()]);
        RV_SET_CSR(mstatus, status & MSTATUS_MIE);
        return nesting;
}
