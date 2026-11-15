/**
 * @file      faults.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program faults end the program, not the system (devplan/programs.md §21).
 *
 * IDF's two panic entry points are wrapped: `xt_unhandled_exception` (an
 * exception) and `panicHandler` (a panic interrupt: here only the stack
 * guard's). When a loaded program's thread faults in its own code, the
 * wrapper points the trap frame at `bmds_fault_stub` and returns, and
 * `vectors.S` restores the frame and `mret`s there: the stub moves SP back
 * inside the thread's stack, and the thread ends its process as killed by
 * the fault's signal. Anything else (a kernel task, a built-in program, a
 * kernel call, an interrupt handler, an overrun past the gap below the
 * stack) goes on to IDF's panic as before.
 *
 * A program in U-mode (devplan/protection.md §14) faults the same way, through usermode.S:
 * its frame is on the thread's kernel stack with `mepc` the trampoline, so nothing need be
 * contained; the stub restarts at the kernel stack's top.
 *
 * What the wrappers rely on (docs/platforms/esp32p4.md §5a): returning
 * from them resumes through the frame (`mepc` included, but not `sp`:
 * `vectors.S` pops the frame by its size); `mstatus` isn't restored, so
 * clearing MPIE here makes `mret` leave interrupts off; the stack guard's
 * interrupt reaches both cores.
 */

#include "running.h"

#include <cstdint>
#include <cstdio>

#include "esp_cpu.h"
#include "esp_private/cache_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/assist_debug_hal.h"
#include "riscv/csr.h"
#include "riscv/encoding.h"
#include "riscv/rvruntime-frames.h"
#include "soc/soc.h"
#include <bmds/log.h>
#include <bmds/processtable.h>
#include <bmds/programfault.h>

extern "C" {
// Our wrap (critical.cpp): the interrupt nesting, as it was at the trap.
BaseType_t xPortInIsrContext(void);

void __real_xt_unhandled_exception(void *frame);
void __real_panicHandler(void *frame);

void              bmds_fault_stub(void);
void              bmds_fault_rearm(void);
[[noreturn]] void bmds_fault_exit(void);
}

// Where a faulting program thread resumes: a0 holds the SP its kernel entry began with, and
// interrupts are off (MPIE was cleared), so nothing lands on the overrun stack before SP is back
// inside it. The guard's monitor goes back on (the wrapper stopped it), then interrupts.
asm(R"(
        .section .text.bmds_fault_stub, "ax", @progbits
        .global  bmds_fault_stub
        .type    bmds_fault_stub, @function
        .align   2
bmds_fault_stub:
        mv      sp, a0
        call    bmds_fault_rearm
        csrsi   mstatus, 8
        call    bmds_fault_exit
        unimp
        .size   bmds_fault_stub, . - bmds_fault_stub
)");

BMDS_NAMESPACE_BEGIN

namespace {

        // A panic interrupt with the guard's number is the guard's: nothing else shares it on the
        // P4 (soc.h allows for it), so "nothing latched" can only mean already cleared.
        static_assert(ETS_ASSIST_DEBUG_INUM != ETS_CACHEERR_INUM, "the guard's interrupt is its own");

        // How long a core the stack guard didn't fire on waits for the one it did to recover
        // (it clears the status within microseconds) before treating it as IDF does.
        constexpr uint32_t GuardWaitCycles = 360000; // 1 ms at 360 MHz

        /**
         * @brief Sends the current task, if it is a loaded program's thread that
         *        faulted in its own code, to the stub; false leaves the trap to IDF.
         *
         * In the trap: interrupts masked, the flash cache on (the wrappers check).
         * No locks and no allocation.
         */
        bool redirect(RvExcFrame &frame, uint32_t cause, int signal) {
                if (xPortInIsrContext() != 0) return false; // an interrupt handler's fault
                ProgramThread *t = ProgramThread::ofCurrentTask();
                if (t == nullptr || t->run == nullptr) return false;     // a kernel task
                if (t->run->builtin != nullptr) return false;            // kernel code throughout
                // In U-mode (devplan/protection.md §14) the entry sent the trap here from the
                // program's code: the frame on the kernel stack, `mepc` the trampoline, the
                // program's PC and SP in the block. Anything else on such a thread is the kernel's.
                const bool user = t->user != nullptr && UserMode::isReturn(static_cast<uint32_t>(frame.mepc));
                if (t->user != nullptr && !user) return false;
                if (!user) {
                        if (t->entrySp == 0 || t->inKernel.load()) return false; // a kernel call, or not started
                        // Everything the program wrote, and the frame, must be in memory the thread owns.
                        if (!ProgramFault::isContained(reinterpret_cast<uintptr_t>(&frame), t->task.stackBottom(),
                                                       t->task.guardBytes()))
                                return false;
                }
                ProgramThread::Fault &f = t->fault;
                f.cause = cause;
                f.pc = user ? t->user->pc : static_cast<uint32_t>(frame.mepc);
                f.value = static_cast<uint32_t>(frame.mtval);
                f.ra = static_cast<uint32_t>(frame.ra);
                f.sp = user ? t->user->sp : static_cast<uint32_t>(frame.sp);
                f.signal = signal;
                f.user = user;
                // From here it ends itself: a killer that suspends it now leaves it to (the
                // handshake, running.h).
                t->inKernel.store(true);
                long gp;
                __asm__("mv %0, gp" : "=r"(gp));
                frame.mepc = reinterpret_cast<long>(&bmds_fault_stub);
                // The stub's SP: in U-mode the kernel stack's top (the block: nothing below it is
                // needed any more), else where the kernel entry began.
                frame.a0 = user ? reinterpret_cast<long>(t->user) : static_cast<long>(t->entrySp);
                frame.gp = gp; // the kernel's
                RV_CLEAR_CSR(mstatus, MSTATUS_MPIE);
                // The monitor sees the wrapper's own frames below the bound: off until the stub has
                // moved SP, and whatever it latched cleared.
                const uint32_t core = static_cast<uint32_t>(esp_cpu_get_core_id());
                assist_debug_hal_sp_mon_disable(core);
                assist_debug_hal_sp_int_clear(core);
                return true;
        }

        // Not inlined into the wrappers: only the cache check needs IRAM.
        __attribute__((noinline)) bool fromException(RvExcFrame &frame) {
                const uint32_t cause = static_cast<uint32_t>(frame.mcause);
                const int      signal = ProgramFault::signalFor(cause);
                return signal != 0 && redirect(frame, cause, signal);
        }

        __attribute__((noinline)) bool fromStackGuard(RvExcFrame &frame) {
                const uint32_t core = static_cast<uint32_t>(esp_cpu_get_core_id());
                if (assist_debug_hal_is_sp_ovf_fired(core)) {
                        return redirect(frame, ETS_ASSIST_DEBUG_INUM, ProgramFault::OverflowSignal);
                }
                // The other core's overflow (one interrupt source, routed to both): wait while it is
                // latched. Recovered, the other core clears it and this one returns; panicking, it
                // stalls this core first. Never latched (cleared before this core looked): return.
                const uint32_t start = esp_cpu_get_cycle_count();
                for (;;) {
                        bool fired = false;
                        for (uint32_t c = 0; c < SOC_CPU_CORES_NUM; ++c)
                                fired = fired || assist_debug_hal_is_sp_ovf_fired(c);
                        if (!fired) return true;
                        if (esp_cpu_get_cycle_count() - start > GuardWaitCycles) return false; // IDF's busy wait
                }
        }

        // "+0x1234" inside the program's image, else the absolute address.
        void describe(char *out, size_t size, const Running &run, uint32_t address) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(run.image);
                if (base != 0 && address >= base && address < base + run.imageBytes) {
                        std::snprintf(out, size, "+0x%x", static_cast<unsigned>(address - base));
                } else {
                        std::snprintf(out, size, "0x%08x", static_cast<unsigned>(address));
                }
                return;
        }

} // namespace

BMDS_NAMESPACE_END

extern "C" {

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame) {
        // Nothing outside IRAM while the cache is off: then the fault is the kernel's.
        if (spi_flash_cache_enabled() && bmds::fromException(*static_cast<RvExcFrame *>(frame))) return;
        __real_xt_unhandled_exception(frame);
        return;
}

void IRAM_ATTR __wrap_panicHandler(void *frame) {
        auto *f = static_cast<RvExcFrame *>(frame);
        // Only the stack guard's: the interrupt watchdog and cache errors go straight on.
        if (f->mcause == ETS_ASSIST_DEBUG_INUM && spi_flash_cache_enabled() && bmds::fromStackGuard(*f)) return;
        __real_panicHandler(frame);
        return;
}

// The stub's, with interrupts still off and SP inside the stack again.
void bmds_fault_rearm(void) {
        assist_debug_hal_sp_mon_enable(static_cast<uint32_t>(esp_cpu_get_core_id()));
        return;
}

// The stub's: an ordinary context on the thread's own stack, inKernel set. Reports the fault and
// ends the process as killed by its signal, as a SIGKILL would (a vfork child with its parent).
void bmds_fault_exit(void) {
        using namespace bmds;
        ProgramThread             &t = ProgramThread::self();
        const ProgramThread::Fault f = t.fault;
        const Running             &run = *t.run;
        char                       pc[16], ra[16], who[24];
        describe(pc, sizeof(pc), run, f.pc);
        describe(ra, sizeof(ra), run, f.ra);
        if (t.tid != run.pid) {
                std::snprintf(who, sizeof(who), "%d/%d", static_cast<int>(run.pid), static_cast<int>(t.tid));
        } else {
                std::snprintf(who, sizeof(who), "%d", static_cast<int>(run.pid));
        }
        const char *name = ProgramFault::nameOf(f.signal);
        const char *path = run.path[0] != '\0' ? run.path : run.command;
        if (f.cause == ETS_ASSIST_DEBUG_INUM) {
                const uintptr_t bottom = f.user ? t.userStackLo : t.task.stackBottom();
                const unsigned  past = f.sp < bottom ? static_cast<unsigned>(bottom - f.sp) : 0;
                bmdsErr("%s[%s]: %s (stack overflow, %u bytes past the bottom) at %s, called from %s", path, who, name,
                        past, pc, ra);
        } else if (f.cause == ProgramFault::IllegalInstruction || f.cause == ProgramFault::Breakpoint) {
                bmdsErr("%s[%s]: %s at %s, called from %s", path, who, name, pc, ra);
        } else {
                bmdsErr("%s[%s]: %s at %s, called from %s, address 0x%08x", path, who, name, pc, ra,
                        static_cast<unsigned>(f.value));
        }
        t.endProcess(ProcessTable::killedStatus(f.signal));
}

} // extern "C"
