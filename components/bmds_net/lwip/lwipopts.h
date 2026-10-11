/**
 * @file      lwipopts.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * lwIP's configuration for BMDS (devplan/network.md §15): no lwIP threads
 * (NO_SYS), everything under the network stack's core lock, the heap and the
 * pools in one block of PSRAM, IPv4 with DHCP, IGMP and loopback. The same
 * file builds the firmware and the host tests.
 */

#pragma once

#include "lwipport.h"

// --- The system: no lwIP threads; BMDS's network task and core lock -----------------------

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0 // everything runs under the core lock
#define LWIP_TIMERS 1
#define LWIP_SOCKET 0 // the kernel's own sockets (§7), on the raw API
#define LWIP_NETCONN 0
#define LWIP_NETIF_API 0
#define LWIP_ASSERT_CORE_LOCKED() bmds_lwip_assert_locked()
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 1 // netif_poll scheduled on the task (lwipport.h)
#define LWIP_RAND() bmds_lwip_rand()

// --- Memory: lwIP's heap is one PSRAM block from boot; the pools come out of it -----------

#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (256 * 1024)
#define LWIP_RAM_HEAP_POINTER bmds_lwip_heap
// With MEMP_MEM_MALLOC these bound nothing (the heap does): lwIP checks them against each other.
#define MEMP_NUM_PBUF 32
#define MEMP_NUM_RAW_PCB 8
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_TCP_PCB_LISTEN 8
#define MEMP_NUM_TCP_SEG 64
#define MEMP_NUM_REASSDATA 8
#define MEMP_NUM_ARP_QUEUE 16
#define MEMP_NUM_IGMP_GROUP 16
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1536
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define PBUF_POOL_FREE_OOSEQ 0 // nothing receives into the pool (received frames are custom pbufs)

// --- Interfaces -----------------------------------------------------------------------------

#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF 1
#define LWIP_LOOPBACK_MAX_PBUFS 16
#define LWIP_SINGLE_NETIF 0
#define LWIP_NUM_NETIF_CLIENT_DATA 0
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
#define ETH_PAD_SIZE 0

// --- IPv4 -----------------------------------------------------------------------------------

#define LWIP_IPV4 1
#define LWIP_IPV6 0 // planned (devplan/network.md §6): ip6 is vendored, not built
#define LWIP_ARP 1
#define ARP_TABLE_SIZE 16
#define ARP_QUEUEING 1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define IP_FORWARD 0
#define IP_REASSEMBLY 1
#define IP_FRAG 1
#define IP_DEFAULT_TTL 64
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_DHCP 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_AUTOIP 0
#define LWIP_IGMP 1
#define LWIP_DNS 1
#define DNS_TABLE_SIZE 4
#define DNS_MAX_NAME_LENGTH 128
#define DNS_MAX_SERVERS 2
#define LWIP_DHCP_MAX_DNS_SERVERS 0 // DHCP's servers through the hook below: net/dns/servers wins
#define DNS_LOOKUP_LOCAL_EXTERN(name, length, address, type) bmds_dns_local((name), (length), (address))
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, type, option, length, p, offset)                          \
        do {                                                                                                           \
                LWIP_UNUSED_ARG(msg);                                                                                  \
                bmds_dhcp_option((netif), (type), (option), (length), (p), (offset));                                  \
        } while (0)
#define LWIP_UDP 1
#define LWIP_UDPLITE 0
#define LWIP_TCP 1
#define LWIP_BROADCAST_PING 1
#define LWIP_MULTICAST_PING 1

// --- TCP ------------------------------------------------------------------------------------

#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + TCP_MSS - 1) / TCP_MSS)
#define TCP_QUEUE_OOSEQ 1
#define TCP_OOSEQ_MAX_PBUFS 4
#define LWIP_TCP_SACK_OUT 1
#define LWIP_WND_SCALE 0
#define TCP_LISTEN_BACKLOG 1
#define LWIP_TCP_KEEPALIVE 1
#define SO_REUSE 1

// --- Statistics: the memory's, for `net` (leaks show there) ---------------------------------

#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 0
#define MEM_STATS 1
#define MEMP_STATS 1
#define LINK_STATS 1
#define ETHARP_STATS 0
#define IP_STATS 0
#define IPFRAG_STATS 0
#define ICMP_STATS 0
#define IGMP_STATS 0
#define UDP_STATS 0
#define TCP_STATS 0
#define SYS_STATS 0

// --- Checksums: in software for now (devplan/network.md §15) --------------------------------

#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_TCP 1
#define CHECKSUM_GEN_ICMP 1
#define CHECKSUM_CHECK_IP 1
#define CHECKSUM_CHECK_UDP 1
#define CHECKSUM_CHECK_TCP 1
#define CHECKSUM_CHECK_ICMP 1
#define LWIP_CHKSUM_ALGORITHM 3

// --- Messages: none (LWIP_DEBUG undefined): errors are returned and `net` shows the state ---
