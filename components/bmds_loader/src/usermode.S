/**
 * @file      usermode.S
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs in U-mode (devplan/protection.md §4.2, §4.3, §14): the stub page a program calls,
 * the way in from U-mode (after trapentry.S's stubs), the system call dispatch, the trampoline
 * (the only way down to U-mode) and a thread's first entry.
 *
 * Every saved task context stays M-mode's: a trap from U-mode moves to the thread's kernel stack
 * first and points `mepc` at the trampoline with MPP = M before IDF's handler sees it. IDF saves
 * its frame there, may switch tasks (`rtos_int_exit` hands back the interrupted task's
 * `mstatus`, right for any task now), and returns to the trampoline, which goes back to U-mode
 * with the program's PC, SP and PMP.
 *
 * The thread's block (UserBlock, usermode.h) sits at its kernel stack's top: SP in the kernel
 * is the block's address when nothing is on that stack, and `mscratch` holds it while the
 * thread runs in U-mode (0 in M-mode, trapentry.S's test).
 *
 * The hardware stack guard watches SP every cycle (esp32p4.md §5a): SP moves between the user's
 * stack and the kernel's only with its monitor stopped, its bounds set for the new stack first.
 */

#include <sdkconfig.h>
#include <bmds/errno.h>
#include <esp_private/hw_stack_guard.h>
#include "usermodeoffsets.h"

#define MSTATUS_MIE  0x00000008
#define MSTATUS_MPIE 0x00000080
#define MSTATUS_MPP  0x00001800
#define MCAUSE_CODE  0x80000fff /* the interrupt bit and the exception code (CLIC: others alias) */
#define CAUSE_ECALL_U 8

#define GUARD_MONITOR  ASSIST_DEBUG_CORE_0_MONITOR_REG
#define GUARD_SP_MIN   (ASSIST_DEBUG_CORE_0_SP_MIN_REG - ASSIST_DEBUG_CORE_0_MONITOR_REG)
#define GUARD_SP_MAX   (ASSIST_DEBUG_CORE_0_SP_MAX_REG - ASSIST_DEBUG_CORE_0_MONITOR_REG)
#define GUARD_CORE_SHIFT 7 /* core 1's registers are 0x80 above core 0's */

#if (ASSIST_DEBUG_CORE_1_MONITOR_REG - ASSIST_DEBUG_CORE_0_MONITOR_REG) != (1 << GUARD_CORE_SHIFT) || \
        (ASSIST_DEBUG_CORE_1_SP_MIN_REG - ASSIST_DEBUG_CORE_0_SP_MIN_REG) != (1 << GUARD_CORE_SHIFT)
#error "the stack guard's registers per core aren't where usermode.S expects"
#endif

#if !CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#error "usermode.S sets the stack guard's bounds: CONFIG_ESP_SYSTEM_HW_STACK_GUARD"
#endif


/* @reg = this core's stack guard monitor register (uses @tmp). */
.macro guard_base reg, tmp
        csrr    \tmp, mhartid
        slli    \tmp, \tmp, GUARD_CORE_SHIFT
        li      \reg, GUARD_MONITOR
        add     \reg, \reg, \tmp
.endm

.macro guard_stop base, tmp
        lw      \tmp, 0(\base)
        andi    \tmp, \tmp, ~ASSIST_DEBUG_SP_SPILL_BITS
        sw      \tmp, 0(\base)
.endm

.macro guard_start base, tmp
        lw      \tmp, 0(\base)
        ori     \tmp, \tmp, ASSIST_DEBUG_SP_SPILL_BITS
        sw      \tmp, 0(\base)
.endm

/* ---------------------------------------------------------------------------------------------
 * The stub page (protection.md §4.3): one 4 KB page of flash code with nothing else in it, the
 * only kernel memory a program may read or run (PMP entry 0). A bmds_sys_t whose slots point at
 * stubs in the same page, each `li t1, n; ecall; ret`: a program calls a slot as before. t1, not
 * a7: every argument register stays the slot's (a0-a7, fa0-fa7).
 * ------------------------------------------------------------------------------------------- */

        /* Placed alone in a 4 KB-aligned page by linker.lf (not by its own alignment: that
         * would move .flash.text). */
        .section .bmds_user_page, "ax"
        .option push
        .option norvc
        .balign 16
        .global bmds_user_page
        .type bmds_user_page, @object
bmds_user_page:
        .word   BMDS_K_SYS_MAGIC
        .word   BMDS_K_ABI_VERSION
        .word   BMDS_K_SLOTS
        .word   0
        .set    slot, 0
        .rept   BMDS_K_SLOTS
        .word   bmds_user_stubs + slot * BMDS_K_STUB_BYTES
        .set    slot, slot + 1
        .endr

        .global bmds_user_stubs
bmds_user_stubs:
        .set    slot, 0
        .rept   BMDS_K_SLOTS
        addi    t1, zero, slot
        ecall
        jalr    zero, 0(ra)
        .set    slot, slot + 1
        .endr
bmds_user_stubs_end:

/* Where a program's _start returns to (its ra): exit with what it returned. */
        .global bmds_user_exit
bmds_user_exit:
        addi    t1, zero, 0                     /* BMDS_SYS_EXIT */
        ecall
        unimp

/* Where a thread's entry returns to: thread_exit(its result, NULL). */
        .global bmds_user_thread_exit
bmds_user_thread_exit:
        addi    a1, zero, 0
        addi    t1, zero, 54                    /* BMDS_SYS_THREAD_EXIT */
        ecall
        unimp

        .size bmds_user_page, . - bmds_user_page
        .option pop

        .if (bmds_user_stubs_end - bmds_user_stubs) != BMDS_K_SLOTS * BMDS_K_STUB_BYTES
        .error "a stub isn't BMDS_K_STUB_BYTES"
        .endif
        .if (bmds_user_thread_exit - bmds_user_page) > BMDS_K_PAGE_BYTES - 16
        .error "the stub page doesn't fit its 4 KB"
        .endif

/* ---------------------------------------------------------------------------------------------
 * In IRAM: an interrupt may come while the flash cache is off (another core's flash write).
 * ------------------------------------------------------------------------------------------- */

        .section .iram1.bmds_user_mode, "ax"

/*
 * A trap from U-mode, from a trapentry.S stub: t0 = the block, `mscratch` = the program's t0,
 * its t1 in the block, t1 = where in the stub to go on to IDF's handler. Saves what the program
 * had in t0, SP, gp and PC, moves to the kernel stack, then: an ecall is a system call; anything
 * else returns to the stub with t0 and SP as IDF's handler needs them (t1 the stub reloads).
 */
        .global bmds_user_entry
        .type bmds_user_entry, @function
bmds_user_entry:
        sw      t1, BMDS_UB_LINK(t0)
        sw      sp, BMDS_UB_SP(t0)
        sw      gp, BMDS_UB_GP(t0)
        csrrw   gp, mscratch, zero              /* gp = the program's t0; M-mode's 0 from here */
        sw      gp, BMDS_UB_T0(t0)
        csrr    gp, mepc
        sw      gp, BMDS_UB_PC(t0)
        /* The kernel stack: [its bottom, the block]. */
        guard_base gp, t1
        guard_stop gp, t1
        lw      t1, BMDS_UB_KERNEL_LO(t0)
        sw      t1, GUARD_SP_MIN(gp)
        sw      t0, GUARD_SP_MAX(gp)
        mv      sp, t0
        guard_start gp, t1
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        csrr    t1, mcause
        li      t0, MCAUSE_CODE
        and     t1, t1, t0
        li      t0, CAUSE_ECALL_U
        beq     t1, t0, bmds_user_ecall
        /* To IDF's handler, which returns to the trampoline in M-mode. */
        la      t1, bmds_user_return
        csrw    mepc, t1
        li      t1, MSTATUS_MPP
        csrs    mstatus, t1
        lw      t1, BMDS_UB_LINK(sp)
        lw      t0, BMDS_UB_T0(sp)
        jr      t1
        .size bmds_user_entry, . - bmds_user_entry

/*
 * A system call (protection.md §4.3): t1 was the slot's number. The slot runs as the thread in
 * ordinary task context (interrupts on, it may block), with the argument registers as the
 * program left them; its result comes back in a0/a1/fa0. The stub's own ra is kept.
 */
        .type bmds_user_ecall, @function
bmds_user_ecall:
        /* Out of the ecall's trap first, by an `mret` with this trap's own mcause, into M-mode
         * (interrupts back on): every trap leaves through its own `mret` (the CLIC's rule: see
         * bmds_user_return). The slot then runs as ordinary kernel code. */
        la      t0, 1f
        csrw    mepc, t0
        li      t0, MSTATUS_MPP
        csrs    mstatus, t0
        mret
1:      lw      t0, BMDS_UB_PC(sp)
        addi    t0, t0, 4                       /* past the ecall */
        sw      t0, BMDS_UB_PC(sp)
        sw      ra, BMDS_UB_RA(sp)
        lw      t1, BMDS_UB_T1(sp)
        la      t0, bmds_k_table
        lw      t2, BMDS_K_SYS_COUNT(t0)
        bgeu    t1, t2, 3f
        slli    t1, t1, 2
        add     t0, t0, t1
        lw      t0, BMDS_K_SYS_SLOTS(t0)
        beqz    t0, 3f
        csrsi   mstatus, MSTATUS_MIE
        jalr    t0
        lw      ra, BMDS_UB_RA(sp)
        j       bmds_user_return
3:      li      a0, -BMDS_ENOSYS
        lw      ra, BMDS_UB_RA(sp)
        j       bmds_user_return
        .size bmds_user_ecall, . - bmds_user_ecall

/*
 * The trampoline: back to U-mode at the block's PC, from the thread's kernel stack (SP = the
 * block), in M-mode, any register but t0-t2, SP, gp and the PC the program's (or a system call's
 * result). Loads this core's PMP with the process's set unless it holds it already (a set's
 * stamp is even and unique; odd while it is rewritten, which the other core does with its
 * interrupts off: a moment).
 */
        .global bmds_user_return
        .type bmds_user_return, @function
bmds_user_return:
        csrci   mstatus, MSTATUS_MIE
        sw      t0, BMDS_UB_T0(sp)
        sw      t1, BMDS_UB_T1(sp)
        sw      t2, BMDS_UB_T2(sp)
        lw      gp, BMDS_UB_PMP(sp)             /* gp = the PmpSet */
        csrr    t2, mhartid
        slli    t2, t2, 2
        la      t0, bmds_pmp_loaded
        add     t2, t2, t0                      /* t2 = &bmds_pmp_loaded[core] */
1:      lw      t1, BMDS_PMP_STAMP(gp)
        andi    t0, t1, 1
        bnez    t0, 1b
        lw      t0, 0(t2)
        beq     t0, t1, 2f
        lw      t0, BMDS_PMP_CFG + 0(gp)
        csrw    pmpcfg0, t0
        lw      t0, BMDS_PMP_CFG + 4(gp)
        csrw    pmpcfg1, t0
        lw      t0, BMDS_PMP_CFG + 8(gp)
        csrw    pmpcfg2, t0
        lw      t0, BMDS_PMP_CFG + 12(gp)
        csrw    pmpcfg3, t0
        .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        lw      t0, BMDS_PMP_ADDR + 4 * \n(gp)
        csrw    pmpaddr\n, t0
        .endr
        fence
        lw      t0, BMDS_PMP_STAMP(gp)
        bne     t0, t1, 1b                      /* rewritten meanwhile: again */
        sw      t1, 0(t2)
        /* The return itself is an `mret` out of a trap of its own: on this CLIC an `mret` whose
         * mcause.interrupt doesn't match the entry type of the trap it leaves corrupts the CLIC's
         * state (after one, the core took no more interrupts, or with mcause cleared no more
         * illegal-instruction exceptions, so the lazy FPU/PIE enable of the next task stalled the
         * core: esp32p4.md §5a). Here there is no trap to leave (IDF's handler has already
         * returned, or this is a thread's first entry), so it makes one: an `ecall` from M-mode,
         * which trapentry.S's exception stub sends to bmds_user_resume. */
2:      ecall
        unimp
        .size bmds_user_return, . - bmds_user_return

/*
 * The trampoline's own trap (an `ecall` from M-mode, trapentry.S): SP = the block, MIE off,
 * mcause this trap's (an exception's, level 0), t0 and `mscratch` scratch. Into U-mode.
 */
        .global bmds_user_resume
        .type bmds_user_resume, @function
bmds_user_resume:
        lw      t0, BMDS_UB_PC(sp)
        csrw    mepc, t0
        li      t0, MSTATUS_MPP
        csrc    mstatus, t0                     /* MPP = U */
        li      t0, MSTATUS_MPIE
        csrs    mstatus, t0
        csrw    mscratch, sp
        /* The program's stack: [its bottom, its top]. */
        guard_base t2, t1
        guard_stop t2, t1
        lw      t1, BMDS_UB_USER_LO(sp)
        sw      t1, GUARD_SP_MIN(t2)
        lw      t1, BMDS_UB_USER_HI(sp)
        sw      t1, GUARD_SP_MAX(t2)
        mv      t0, sp
        lw      sp, BMDS_UB_SP(t0)
        guard_start t2, t1
        lw      gp, BMDS_UB_GP(t0)
        lw      t2, BMDS_UB_T2(t0)
        lw      t1, BMDS_UB_T1(t0)
        lw      t0, BMDS_UB_T0(t0)
        mret
        .size bmds_user_resume, . - bmds_user_resume

/*
 * bmds_user_start(block, a0, a1, a2, a3, ra, tp): a thread's first entry to U-mode, from its
 * kernel code; never returns. SP becomes the block (the kernel stack's top from here on); the
 * program gets a0-a3, ra and tp, every other register clear (the block's t0-t2 are 0).
 */
        .global bmds_user_start
        .type bmds_user_start, @function
bmds_user_start:
        mv      sp, a0
        mv      ra, a5
        mv      tp, a6
        mv      a0, a1
        mv      a1, a2
        mv      a2, a3
        mv      a3, a4
        li      a4, 0
        li      a5, 0
        li      a6, 0
        li      a7, 0
        li      s0, 0
        li      s1, 0
        li      s2, 0
        li      s3, 0
        li      s4, 0
        li      s5, 0
        li      s6, 0
        li      s7, 0
        li      s8, 0
        li      s9, 0
        li      s10, 0
        li      s11, 0
        li      t3, 0
        li      t4, 0
        li      t5, 0
        li      t6, 0
        j       bmds_user_return
        .size bmds_user_start, . - bmds_user_start
