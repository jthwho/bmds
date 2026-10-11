/**
 * @file      cc.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * lwIP's compiler and platform definitions for BMDS (GCC, little endian;
 * the ESP32-P4 and the host tests alike).
 */

#pragma once

#include <stdlib.h>

#define LWIP_TIMEVAL_PRIVATE 0

#define lwip_htons(x) ((u16_t)__builtin_bswap16((u16_t)(x)))
#define lwip_htonl(x) ((u32_t)__builtin_bswap32((u32_t)(x)))

/* No messages (LWIP_DEBUG is undefined); a failed assertion stops the system. */
#define LWIP_PLATFORM_DIAG(x)                                                                                          \
        do {                                                                                                           \
        } while (0)
#define LWIP_PLATFORM_ASSERT(x) bmds_lwip_assert((x), __FILE__, __LINE__)
