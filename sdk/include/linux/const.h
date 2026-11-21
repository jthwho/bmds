/**
 * @file      const.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's constant helpers (`include/uapi/linux/const.h`) as BMDS provides them, for Linux's
 * uapi headers. Written for BMDS; the names are Linux's.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_CONST_H
#define BMDS_LINUX_CONST_H

#ifdef __linux__
/* Compiled on Linux (a host's tests): Linux's own, which the C library's headers include too. */
#include_next <linux/const.h>
#else

#define _AC(X, Y) (X##Y)
#define _AT(T, X) ((T)(X))
#define _UL(x) (_AC(x, UL))
#define _ULL(x) (_AC(x, ULL))
#define _BITUL(x) (_UL(1) << (x))
#define _BITULL(x) (_ULL(1) << (x))

#define __ALIGN_KERNEL(x, a) __ALIGN_KERNEL_MASK(x, (__typeof__(x))(a) - 1)
#define __ALIGN_KERNEL_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#endif /* __linux__ */

#endif /* BMDS_LINUX_CONST_H */
