/**
 * @file      types.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's userspace types (`include/uapi/linux/types.h`, `asm-generic/int-ll64.h`) as BMDS
 * provides them, for ported code and Linux's own uapi headers (libgpiod carries one). Written
 * for BMDS; the names and sizes are Linux's.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_TYPES_H
#define BMDS_LINUX_TYPES_H

#include <stdint.h>

typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;

/* Byte order marked, as Linux's sparse annotations (none here: plain integers). */
#define __bitwise
typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;
typedef __u16 __bitwise __sum16;
typedef __u32 __bitwise __wsum;

/* 64-bit fields aligned to 8 bytes on 32-bit machines too, as structures shared with 64-bit
   kernels lay them out. */
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define __aligned_s64 __s64 __attribute__((aligned(8)))
#define __aligned_be64 __be64 __attribute__((aligned(8)))
#define __aligned_le64 __le64 __attribute__((aligned(8)))

typedef unsigned __bitwise __poll_t;

#endif /* BMDS_LINUX_TYPES_H */
