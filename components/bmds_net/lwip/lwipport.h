/**
 * @file      lwipport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What lwIP needs from BMDS (devplan/network.md §15), as C: the heap's
 * block, the core lock's check, random numbers, and calls queued for the
 * network task. The network stack defines them on the target, the host
 * tests their own.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief lwIP's heap: `MEM_SIZE` bytes (and alignment), set before `lwip_init()`. */
extern unsigned char *bmds_lwip_heap;

/** @brief Asserts that the calling task holds the core lock (`LWIP_ASSERT_CORE_LOCKED`). */
void bmds_lwip_assert_locked(void);

/** @brief A random number (`LWIP_RAND`). */
unsigned int bmds_lwip_rand(void);

/** @brief lwIP's assertion failed (`LWIP_PLATFORM_ASSERT`). Does not return. */
void bmds_lwip_assert(const char *message, const char *file, int line) __attribute__((noreturn));

/** @brief A call for the network task (lwIP's `tcpip_callback_fn`, which NO_SYS lacks). */
typedef void (*tcpip_callback_fn)(void *ctx);

/**
 * @brief Queues @p fn(@p ctx) to run on the network task under the core lock, without
 *        waiting: what lwIP's loopback asks of `tcpip_try_callback()`.
 * @return 0 (`ERR_OK`), or -1 (`ERR_MEM`) when the queue is full.
 */
int bmds_lwip_schedule(tcpip_callback_fn fn, void *ctx);

#define tcpip_try_callback(fn, ctx) ((err_t)bmds_lwip_schedule((fn), (ctx)))

#ifdef __cplusplus
}
#endif
