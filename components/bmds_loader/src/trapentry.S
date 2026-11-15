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
#include <soc/soc.h>

#define BMDS_STORM_MAGIC 0x4d525453 /* "STRM": trapentry.cpp checks it */

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
        /* IDF's panic handler pushes its frame at SP: an SP no stack can be at would fault there,
         * again and again, both cores silent until the hardware watchdog (docs/platforms/
         * esp32p4.md §5a). Such an SP goes to bmds_trap_bad_sp instead. Stacks can be in TCM, in
         * PSRAM, in internal RAM past IRAM's code, or in LP RAM (the RTC heap): in address order.
         * (An SP wrongly called bad would come back from a harmless trap on the emergency stack.) */
        li      t0, SOC_TCM_LOW
        bleu    sp, t0, bmds_trap_bad_sp
        li      t0, SOC_TCM_HIGH
        bleu    sp, t0, 2f
        li      t0, SOC_EXTRAM_LOW
        bleu    sp, t0, bmds_trap_bad_sp
        li      t0, SOC_EXTRAM_HIGH
        bleu    sp, t0, 2f
        lui     t0, %hi(_iram_end)
        addi    t0, t0, %lo(_iram_end)
        bleu    sp, t0, bmds_trap_bad_sp
        li      t0, SOC_DRAM_HIGH
        bleu    sp, t0, 2f
        li      t0, SOC_RTC_DRAM_LOW
        bleu    sp, t0, bmds_trap_bad_sp
        li      t0, SOC_RTC_DRAM_HIGH
        bgtu    sp, t0, bmds_trap_bad_sp
2:      csrrw   t0, mscratch, zero              /* t0 back, `mscratch` 0 again */
        j       _panic_handler
1:      sw      t1, BMDS_UB_T1(t0)
        jal     t1, bmds_user_entry
        lw      t1, BMDS_UB_T1(sp)
        j       _panic_handler
        .size bmds_trap_exception, . - bmds_trap_exception

/* An exception in M-mode with an SP no stack can be at (t0 free, the trapped t0 in `mscratch`):
 * the first one's mepc, mcause, mtval, SP and ra kept in RTC memory for the next boot to report
 * (trapentry.cpp), then IDF's panic on this core's emergency stack, so it can say what happened.
 * The trapped t0 is lost (the bad SP takes its place in the report). A third on a core resets. */
        .global bmds_trap_bad_sp
        .type bmds_trap_bad_sp, @function
bmds_trap_bad_sp:
        csrw    mscratch, sp                    /* the bad SP; sp is a scratch register now */
        csrr    t0, mhartid
        slli    t0, t0, 5                       /* a record is 32 bytes */
        la      sp, bmds_storm_records
        add     t0, t0, sp
        csrr    sp, mscratch
        sw      sp, 12(t0)
        sw      ra, 16(t0)
        csrr    sp, mepc
        sw      sp, 0(t0)
        csrr    sp, mcause
        sw      sp, 4(t0)
        csrr    sp, mtval
        sw      sp, 8(t0)
        lw      sp, 20(t0)
        addi    sp, sp, 1
        sw      sp, 20(t0)
        li      sp, BMDS_STORM_MAGIC
        sw      sp, 24(t0)
        lw      t0, 20(t0)
        addi    t0, t0, -3                      /* >= 0: the third on this core */
        csrr    sp, mhartid
        slli    sp, sp, 11                      /* 2 KB a core */
        addi    sp, sp, 2047
        addi    sp, sp, 1
        bgez    t0, 3f
        la      t0, bmds_storm_stacks
        add     sp, sp, t0                      /* this core's emergency stack's top */
        csrrw   t0, mscratch, zero              /* t0: the bad SP, for the report */
        j       _panic_handler
3:      la      t0, bmds_storm_stacks
        add     sp, sp, t0
        tail    esp_rom_software_reset_system   /* storming still: start again */
        .size bmds_trap_bad_sp, . - bmds_trap_bad_sp

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
