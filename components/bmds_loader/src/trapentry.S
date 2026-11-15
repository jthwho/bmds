/**
 * @file      trapentry.S
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS's trap entry in front of ESP-IDF's (devplan/protection.md §4.2): `mtvec` and every
 * vector of `mtvt` lead here first (trapentry.cpp installs them). One stub per place IDF's own
 * table leads to: `_panic_handler` (exceptions, the system interrupts, the panic interrupts),
 * `_interrupt_handler` (the rest), and `esp_ipc_isr_handler` (the IPC interrupt).
 *
 * Each asks first where the trap came from: `mscratch` is 0 while a core runs M-mode code and the
 * running thread's kernel-side block while it runs U-mode code. `csrrw t0, mscratch, t0` swaps it
 * into t0: a zero is a trap from M-mode, swapped back at once and passed on unchanged, with every
 * register as it was (three instructions). A block is a trap from U-mode: usermode.S's
 * `bmds_user_entry` moves to the thread's kernel stack, takes a system call itself, and returns
 * here for anything else, which goes on to IDF's handler as from M-mode (`mepc` the trampoline).
 *
 * **Not SP:** the usual trick swaps SP with `mscratch`, but the hardware stack guard watches SP
 * every cycle (M-mode and U-mode alike), and SP at 0 for one instruction latched it: the first
 * interrupt panicked "Stack protection fault" in the IPC task (2026-11-14). SP moves only with
 * the guard's monitor stopped (usermode.S).
 *
 * The vector table is here too, in IRAM's code (`bmds_trap_table`), as IDF keeps its own. Built in
 * internal data RAM (read and write only since BMDS's PMA, devplan/protection.md §12), the
 * firmware crashed at boot until recovery took over (2026-11-14): most likely the CLIC's load of a
 * vector's address needs execute permission (protection.md §13). So the table is fixed at link time, from
 * the same options as IDF's (riscv/vectors_clic.S), and trapentry.cpp checks it against IDF's
 * before installing: one that doesn't match leaves IDF's vectors in place.
 *
 * In IRAM: an interrupt may come while the flash cache is off.
 */

#include <sdkconfig.h>
#include "usermodeoffsets.h"

        .section .iram1.bmds_trap_entry, "ax"

/* A stub: from M-mode, straight on to IDF's @target; from U-mode, through bmds_user_entry, which
 * comes back (unless it was a system call) with SP the kernel stack and every other register the
 * program's but t1, reloaded here. */
.macro bmds_trap_stub name, target
        .global \name
        .type \name, @function
\name:
        csrrw   t0, mscratch, t0
        bnez    t0, 1f
        csrrw   t0, mscratch, t0
        j       \target
1:      sw      t1, BMDS_UB_T1(t0)
        jal     t1, bmds_user_entry
        lw      t1, BMDS_UB_T1(sp)
        j       \target
        .size \name, . - \name
.endm

        /* mtvec's base: its low 6 bits are the mode's, so the entry is 64-aligned. Exceptions
         * from M-mode also ask whether this is the U-mode trampoline's own `ecall` (cause 11:
         * nothing else in M-mode issues one), its way into U-mode (usermode.S). */
        .balign 64
        .global bmds_trap_exception
        .type bmds_trap_exception, @function
bmds_trap_exception:
        csrrw   t0, mscratch, t0
        bnez    t0, 1f
        csrr    t0, mcause
        slli    t0, t0, 20                      /* the exception code: bits 11-0 */
        srli    t0, t0, 20
        addi    t0, t0, -11
        beqz    t0, bmds_user_resume
        csrrw   t0, mscratch, zero              /* t0 back, `mscratch` 0 again */
        j       _panic_handler
1:      sw      t1, BMDS_UB_T1(t0)
        jal     t1, bmds_user_entry
        lw      t1, BMDS_UB_T1(sp)
        j       _panic_handler
        .size bmds_trap_exception, . - bmds_trap_exception

        .balign 4
        bmds_trap_stub bmds_trap_irq_interrupt, _interrupt_handler

        .balign 4
        bmds_trap_stub bmds_trap_irq_panic, _panic_handler

#if CONFIG_ESP_IPC_ISR_ENABLE
        .balign 4
        bmds_trap_stub bmds_trap_irq_ipc, esp_ipc_isr_handler
#endif

/* mtvt: vector i's handler address, as IDF's _mtvt_table but through the stubs above. (An
 * interrupt taken in U-mode fetches its vector with M-mode's rights: U-mode isn't granted it.) */
        .balign 64
        .global bmds_trap_table
        .type bmds_trap_table, @object
bmds_trap_table:
        .rept 16                                /* 0-15: the system interrupts */
        .word bmds_trap_irq_panic
        .endr
        .rept 24                                /* 16-39: free */
        .word bmds_trap_irq_interrupt
        .endr
        .word bmds_trap_irq_panic               /* 40: ETS_INT_WDT_INUM */
        .word bmds_trap_irq_panic               /* 41: ETS_CACHEERR_INUM */
        .word bmds_trap_irq_interrupt           /* 42: ETS_MEMPROT_ERR_INUM */
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
        .word bmds_trap_irq_panic               /* 43: ETS_ASSIST_DEBUG_INUM, the stack guard */
#else
        .word bmds_trap_irq_interrupt
#endif
#if CONFIG_ESP_IPC_ISR_ENABLE
        .word bmds_trap_irq_ipc                 /* 44: ETS_IPC_ISR_INUM */
#else
        .word bmds_trap_irq_interrupt
#endif
        .rept 3                                 /* 45-47: free */
        .word bmds_trap_irq_interrupt
        .endr
        .size bmds_trap_table, . - bmds_trap_table
