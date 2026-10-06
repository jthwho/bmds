/*
 * @file      syscall_cp.S
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * musl's __syscall_cp_asm (a cancellation point's system call: musl's own,
 * src/thread/riscv32/syscall_cp.s, issues ecall). Linked only with
 * pthread_cancel.c, which calls it: if the thread's cancel flag is set it
 * is cancelled (__cancel), else the call goes to __bmds_syscall. musl's
 * SIGCANCEL handler would look at __cp_begin and __cp_end; BMDS never sends
 * that signal (a cancelled thread blocked in a call is woken with EINTR
 * instead: BMDS_THREAD_WAKE), so asynchronous cancellation never acts here.
 */

        .text

/* long __syscall_cp_asm(volatile int *cancel, long nr, u, v, w, x, y, z): a0-a7. */
        .globl  __syscall_cp_asm
        .hidden __syscall_cp_asm
        .type   __syscall_cp_asm, @function
        .globl  __cp_begin
        .hidden __cp_begin
        .globl  __cp_end
        .hidden __cp_end
        .globl  __cp_cancel
        .hidden __cp_cancel
__syscall_cp_asm:
__cp_begin:
        lw      t0, 0(a0)
        bnez    t0, __cp_cancel
        mv      a0, a1
        mv      a1, a2
        mv      a2, a3
        mv      a3, a4
        mv      a4, a5
        mv      a5, a6
        mv      a6, a7
        tail    __bmds_syscall
__cp_end:
__cp_cancel:
        tail    __cancel
        .size   __syscall_cp_asm, . - __syscall_cp_asm

        .section .note.GNU-stack, "", @progbits
