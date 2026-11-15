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

#include <algorithm>
#include <cstddef>
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
                f.value = static_cast<uint32_t>(frame.mtval);
                // The frame begins with mepc, then x1-x31 in order (rvruntime-frames.h). From U-mode,
                // its PC, SP and gp are in the block (the entry moved to the kernel's).
                static_assert(offsetof(RvExcFrame, ra) == 4 && offsetof(RvExcFrame, t6) == 31 * 4,
                              "the frame holds the registers in order");
                const long *regs = &frame.mepc;
                for (unsigned i = 0; i < 32; ++i) f.regs[i] = static_cast<uint32_t>(regs[i]);
                if (user) {
                        f.regs[0] = t->user->pc;
                        f.regs[2] = t->user->sp;
                        f.regs[3] = t->user->gp;
                }
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

        bool inImage(const Running &run, uint32_t address) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(run.image);
                return base != 0 && address >= base && address < base + run.imageBytes;
        }

        // "+0x1234" inside the program's image, else the absolute address.
        void describe(char *out, size_t size, const Running &run, uint32_t address) {
                if (inImage(run, address)) {
                        std::snprintf(out, size, "+0x%x",
                                      static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(run.image)));
                } else {
                        std::snprintf(out, size, "0x%08x", static_cast<unsigned>(address));
                }
                return;
        }

        // True if @p r could be a return address in the program: in its image, just past a call that
        // links ra (jal/jalr ra, or c.jal/c.jalr).
        bool isReturn(const Running &run, uint32_t r) {
                if ((r & 1u) != 0 || !inImage(run, r - 4) || !inImage(run, r - 1)) return false;
                const auto    *h = reinterpret_cast<const uint16_t *>(r);
                const uint32_t insn = h[-2] | (uint32_t(h[-1]) << 16);
                if ((insn & 3u) == 3u) {
                        if ((insn & 0xfffu) == 0x0efu) return true;   // jal ra, ...
                        if ((insn & 0x7fffu) == 0x00e7u) return true; // jalr ra, ...
                }
                const uint16_t c = h[-1];
                if ((c & 0xf07fu) == 0x9002u && (c & 0x0f80u) != 0) return true; // c.jalr rs1
                return (c & 0xe003u) == 0x2001u;                                 // c.jal (RV32)
        }

        constexpr unsigned TraceDepth = 16;        // return addresses reported
        constexpr uint32_t TraceScanBytes = 16384; // of the stack, from SP up

        // The stack the program's code ran on: [lo, hi).
        void stackOf(const ProgramThread &t, const ProgramThread::Fault &f, uintptr_t &lo, uintptr_t &hi) {
                if (f.user) {
                        lo = t.userStackLo;
                        hi = t.userStackHi;
                } else {
                        lo = t.task.stackBottom();
                        hi = t.entrySp;
                }
                return;
        }

        // The log's stack trace: the PC, then what looks like a return address on the stack, from SP
        // up. Programs keep no frame pointer (-fomit-frame-pointer), so this is a scan: an address
        // left by a call that has returned may show up too. tools/bmds-fault names them. Lines go
        // "  WHO: trace ..." (a log line is 160 bytes), eight addresses to a line.
        void reportTrace(const char *who, const ProgramThread &t, const Running &run, const ProgramThread::Fault &f) {
                uint32_t  found[TraceDepth];
                unsigned  n = 0;
                uintptr_t lo = 0, hi = 0;
                stackOf(t, f, lo, hi);
                found[n++] = f.pc();
                // ra first: a leaf function never stores it.
                if (isReturn(run, f.ra())) found[n++] = f.ra();
                bool            skippedRa = false;
                uintptr_t       a = (std::max<uintptr_t>(f.sp(), lo) + 3u) & ~uintptr_t(3);
                const uintptr_t end = std::min<uintptr_t>(hi, a + TraceScanBytes);
                for (; a + 4 <= end && n < TraceDepth; a += 4) {
                        const uint32_t w = *reinterpret_cast<const uint32_t *>(a);
                        // ra's own saved copy once (it is listed already); a recursion's repeats stay.
                        if (w == f.ra() && !skippedRa) {
                                skippedRa = true;
                                continue;
                        }
                        if (isReturn(run, w)) found[n++] = w;
                }
                for (unsigned row = 0; row < n; row += 8) {
                        char   line[8 * 12 + 1];
                        size_t used = 0;
                        for (unsigned i = row; i < n && i < row + 8; ++i) {
                                char one[16];
                                describe(one, sizeof(one), run, found[i]);
                                const int w = std::snprintf(line + used, sizeof(line) - used, " %s", one);
                                if (w > 0 && size_t(w) < sizeof(line) - used) used += size_t(w);
                        }
                        bmdsErr("  %s: trace%s", who, line);
                }
                return;
        }

        // The registers, eight to a line (x0's place is the PC's).
        void reportRegisters(const char *who, const ProgramThread::Fault &f) {
                static constexpr const char *Names[32] = {"pc", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
                                                          "s0", "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
                                                          "a6", "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
                                                          "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
                for (unsigned row = 0; row < 32; row += 8) {
                        char   line[8 * 14 + 1];
                        size_t used = 0;
                        for (unsigned i = row; i < row + 8; ++i) {
                                const int w = std::snprintf(line + used, sizeof(line) - used, " %s %08x", Names[i],
                                                            static_cast<unsigned>(f.regs[i]));
                                if (w > 0 && size_t(w) < sizeof(line) - used) used += size_t(w);
                        }
                        bmdsErr("  %s:%s", who, line);
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
        describe(pc, sizeof(pc), run, f.pc());
        describe(ra, sizeof(ra), run, f.ra());
        if (t.tid != run.pid) {
                std::snprintf(who, sizeof(who), "%d/%d", static_cast<int>(run.pid), static_cast<int>(t.tid));
        } else {
                std::snprintf(who, sizeof(who), "%d", static_cast<int>(run.pid));
        }
        const char *name = ProgramFault::nameOf(f.signal);
        const char *path = run.path[0] != '\0' ? run.path : run.command;
        if (f.cause == ETS_ASSIST_DEBUG_INUM) {
                const uintptr_t bottom = f.user ? t.userStackLo : t.task.stackBottom();
                const unsigned  past = f.sp() < bottom ? static_cast<unsigned>(bottom - f.sp()) : 0;
                bmdsErr("%s[%s]: %s (stack overflow, %u bytes past the bottom) at %s, called from %s", path, who, name,
                        past, pc, ra);
        } else if (f.cause == ProgramFault::IllegalInstruction || f.cause == ProgramFault::Breakpoint) {
                bmdsErr("%s[%s]: %s at %s, called from %s", path, who, name, pc, ra);
        } else {
                bmdsErr("%s[%s]: %s at %s, called from %s, address 0x%08x", path, who, name, pc, ra,
                        static_cast<unsigned>(f.value));
        }
        reportTrace(who, t, run, f);
        reportRegisters(who, f);
        t.endProcess(ProcessTable::killedStatus(f.signal));
}

} // extern "C"
