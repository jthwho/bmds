/**
 * @file      lwipport.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What lwIP needs from BMDS (devplan/network.md §15), as C: the heap's
 * block, the core lock's check, random numbers, calls queued for the
 * network task, interfaces by BMDS's names, DHCP's options lwIP skips, and
 * the names the resolver knows itself. The network stack defines them on the target, the host
 * tests their own.
 */

#pragma once

#include <stddef.h>
#ifdef ESP_PLATFORM
#include <sdkconfig.h> // CONFIG_BMDS_NET_HEAP_KB
#endif

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

struct netif;

/**
 * @brief The interface BMDS calls @p name ("eth0", "lo"), or null: lwIP's own names are two
 *        letters and a number. Core lock held.
 */
struct netif *bmds_netif_named(const char *name);

struct pbuf;

/**
 * @brief Option @p option of a DHCP message of type @p type, @p length bytes at @p offset in
 *        @p p, one lwIP doesn't handle (`LWIP_HOOK_DHCP_PARSE_OPTION`): DNS and NTP servers.
 */
void bmds_dhcp_option(struct netif *netif, unsigned int type, unsigned int option, unsigned int length,
                      const struct pbuf *p, unsigned int offset);

/**
 * @brief A name the resolver answers itself (`DNS_LOOKUP_LOCAL_EXTERN`): localhost, the host
 *        name, `net/hosts`. @p address is an `ip_addr_t`.
 * @return 0 with @p address set, or -1.
 */
int bmds_dns_local(const char *name, size_t length, void *address);

#ifdef __cplusplus
}
#endif
