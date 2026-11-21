/**
 * @file      usermodeoffsets.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_loader: what usermode.S and trapentry.S share with C and C++
 * (devplan/protection.md §4.2, §14). Assembler-safe: defines only. usermode.cpp
 * checks each against the structures it describes.
 */

#pragma once

/* The system call table's slots (systable.c; the stub page has one stub each). */
#define BMDS_K_SLOTS 144

/* bmds_sys_t's header (sys.h; usermode.cpp checks them). */
#define BMDS_K_SYS_MAGIC 0x53444d42
#define BMDS_K_ABI_VERSION 1
#define BMDS_K_SYS_COUNT 8  /* bmds_sys_t::count */
#define BMDS_K_SYS_SLOTS 16 /* bmds_sys_t::slot */

/* The stub page: each stub `li t1, n; ecall; ret`, uncompressed. */
#define BMDS_K_STUB_BYTES 12
#define BMDS_K_PAGE_BYTES 4096

/* UserBlock (usermode.h): a program thread's, on its kernel stack. */
#define BMDS_UB_T0 0
#define BMDS_UB_T1 4
#define BMDS_UB_T2 8
#define BMDS_UB_SP 12
#define BMDS_UB_GP 16
#define BMDS_UB_PC 20
#define BMDS_UB_RA 24
#define BMDS_UB_LINK 28
#define BMDS_UB_KERNEL_LO 32
#define BMDS_UB_USER_LO 36
#define BMDS_UB_USER_HI 40
#define BMDS_UB_PMP 44
#define BMDS_UB_SPACE 48
#define BMDS_UB_BYTES 64

/* clock_get's slot: answered inside its own trap (usermode.S, protection.md §18). */
#define BMDS_K_SYS_CLOCK_GET 23

/* PmpSet (bmds/pmpset.h). */
#define BMDS_PMP_STAMP 0
#define BMDS_PMP_CFG 4
#define BMDS_PMP_ADDR 20
