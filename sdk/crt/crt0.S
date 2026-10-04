/*
 * crt0.S: the program's entry point.
 *
 * Copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * _start(sys, argc, argv, envp): a0-a3 come from the kernel (bmds/sys.h).
 * gp is the kernel's and is never touched. tp is pointed at the program's
 * thread-local block (picolibc keeps errno there), then C takes over.
 */

        .section .text.bmds_start, "ax"
        .globl  _start
        .type   _start, @function
_start:
        lla     t0, __tls_base
        mv      tp, t0
        tail    __bmds_start
        .size   _start, . - _start
