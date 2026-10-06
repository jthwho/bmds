/*
 * @file      vfork.S
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * vfork() and musl's __clone() for CLONE_VM|CLONE_VFORK (devplan/programs.md
 * §13). The child runs in the calling task, on the caller's stack, until it
 * execs or exits; then the C side
 * (linux.c) calls __bmds_vfork_resume(), which returns from vfork or
 * __clone a second time, in the parent, with the child's id.
 *
 * What survives a call is saved in the room __bmds_vfork_prepare gives
 * (linux.c, on the heap with the child's tables; each thread vforks on its
 * own): ra, sp, s0-s11, fs0-fs11 (26 words; single-precision float
 * registers, ilp32f). vfork has no stack frame of its own, so the caller's
 * frame is all the parent needs back, as with Linux's vfork.
 *
 * __clone for a thread (devplan/processes.md §3.10) goes to the C side,
 * __bmds_thread_clone.
 */

#if __riscv_flen != 32
#error "vfork.S saves fs0-fs11 as single precision (ilp32f)"
#endif

#define CLONE_VM 0x100
#define CLONE_VFORK 0x4000

        .text

/* t6 = the room for the registers (__bmds_vfork_prepare), or a branch to
 * \fail (too deep, or no memory). a0-a3 and ra are kept, on the stack
 * across the call (the child's frames aren't there yet). */
.macro prepare fail
        addi    sp, sp, -32
        sw      ra, 28(sp)
        sw      a0, 24(sp)
        sw      a1, 20(sp)
        sw      a2, 16(sp)
        sw      a3, 12(sp)
        call    __bmds_vfork_prepare
        mv      t6, a0
        lw      ra, 28(sp)
        lw      a0, 24(sp)
        lw      a1, 20(sp)
        lw      a2, 16(sp)
        lw      a3, 12(sp)
        addi    sp, sp, 32
        beqz    t6, \fail
.endm

/* Saves the registers at t6 (ra from t0). */
.macro save_registers
        sw      t0, 0(t6)
        sw      sp, 4(t6)
        sw      s0, 8(t6)
        sw      s1, 12(t6)
        sw      s2, 16(t6)
        sw      s3, 20(t6)
        sw      s4, 24(t6)
        sw      s5, 28(t6)
        sw      s6, 32(t6)
        sw      s7, 36(t6)
        sw      s8, 40(t6)
        sw      s9, 44(t6)
        sw      s10, 48(t6)
        sw      s11, 52(t6)
        fsw     fs0, 56(t6)
        fsw     fs1, 60(t6)
        fsw     fs2, 64(t6)
        fsw     fs3, 68(t6)
        fsw     fs4, 72(t6)
        fsw     fs5, 76(t6)
        fsw     fs6, 80(t6)
        fsw     fs7, 84(t6)
        fsw     fs8, 88(t6)
        fsw     fs9, 92(t6)
        fsw     fs10, 96(t6)
        fsw     fs11, 100(t6)
.endm

/* pid_t vfork(void) */
        .globl  vfork
        .type   vfork, @function
vfork:
        prepare 1f
        mv      t0, ra
        save_registers
        mv      s1, t6                  /* survives the call (saved above) */
        call    __bmds_vfork_enter      /* 0: now the child; or -errno (the room stays) */
        lw      ra, 0(s1)
        lw      s1, 12(s1)
        bnez    a0, 2f
        ret
1:      li      a0, -11                 /* EAGAIN: vforks nested too deep, or no memory */
2:      tail    __syscall_ret           /* -1 with errno set */
        .size   vfork, . - vfork

/* int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...):
 * the child calls fn(arg); returns the child's id, or -errno. A thread
 * (not CLONE_VFORK) goes to __bmds_thread_clone with all the arguments.
 * A vfork child is not on the
 * stack given: musl's posix_spawn passes 5 KB in its own frame, enough
 * where exec runs on a kernel stack, but BMDS's exec runs on the caller's.
 * Below the caller's frames there is the whole program stack, and the
 * parent is suspended until the child execs or exits. */
        .globl  __clone
        .type   __clone, @function
__clone:
        li      t1, CLONE_VM | CLONE_VFORK
        and     t2, a2, t1
        bne     t2, t1, 3f
        prepare 4f
        mv      t0, ra
        save_registers
        mv      s1, t6
        mv      s2, a0                  /* fn */
        mv      s4, a3                  /* arg */
        call    __bmds_vfork_enter
        bnez    a0, 5f
        mv      a0, s4
        jalr    s2
        call    __bmds_vfork_child_exit /* with fn's result; does not return */
3:      tail    __bmds_thread_clone     /* a thread; ENOSYS for fork-like clones */
4:      li      a0, -11
        ret
5:      lw      ra, 0(s1)
        lw      s2, 16(s1)
        lw      s4, 24(s1)
        lw      s1, 12(s1)
        ret
        .size   __clone, . - __clone

/* _Noreturn void __bmds_vfork_resume(long pid, const uint32_t *regs):
 * back to where vfork or __clone saved regs, returning pid there. regs may
 * be on the stack below the parent's sp: sp is loaded last. */
        .globl  __bmds_vfork_resume
        .type   __bmds_vfork_resume, @function
__bmds_vfork_resume:
        lw      ra, 0(a1)
        lw      s0, 8(a1)
        lw      s1, 12(a1)
        lw      s2, 16(a1)
        lw      s3, 20(a1)
        lw      s4, 24(a1)
        lw      s5, 28(a1)
        lw      s6, 32(a1)
        lw      s7, 36(a1)
        lw      s8, 40(a1)
        lw      s9, 44(a1)
        lw      s10, 48(a1)
        lw      s11, 52(a1)
        flw     fs0, 56(a1)
        flw     fs1, 60(a1)
        flw     fs2, 64(a1)
        flw     fs3, 68(a1)
        flw     fs4, 72(a1)
        flw     fs5, 76(a1)
        flw     fs6, 80(a1)
        flw     fs7, 84(a1)
        flw     fs8, 88(a1)
        flw     fs9, 92(a1)
        flw     fs10, 96(a1)
        flw     fs11, 100(a1)
        lw      sp, 4(a1)
        ret
        .size   __bmds_vfork_resume, . - __bmds_vfork_resume

        .section .note.GNU-stack, "", @progbits
