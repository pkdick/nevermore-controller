#pragma once

// lwIP config for the Wi-Fi transport (`NEVERMORE_WIFI`).
// Threaded (FreeRTOS) build: BSD-ish sockets API, DHCP, DNS, mDNS.
// See https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html

#define NO_SYS 0
#define LWIP_SOCKET 1
#define LWIP_NETCONN 1
// Don't define `socket`, `close`, `select`, etc. as macros. They collide w/ C++ code.
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_TIMEVAL_PRIVATE 0  // newlib provides `struct timeval`
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_TCP_KEEPALIVE 1

// memory
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE (16 * 1024)
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define MEMP_NUM_NETCONN 12  // GATT TCP (listener + client) + portal (listener + clients)
#define MEMP_NUM_UDP_PCB 8   // DHCP & DNS clients, mDNS, hotspot DHCP & DNS servers
#define MEMP_NUM_TCP_PCB 12  // includes TIME_WAIT leftovers of portal requests
#define MEMP_NUM_SYS_TIMEOUT (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)  // + mDNS
#define PBUF_POOL_SIZE 24

// protocols
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_DHCP 1
#define LWIP_IGMP 1  // mDNS
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// netif
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1  // mDNS

// mDNS responder (`<host>.local`)
#define LWIP_MDNS_RESPONDER 1
#define MDNS_MAX_SERVICES 1

#define LWIP_CHKSUM_ALGORITHM 3

// stats & debug
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
#define LWIP_STATS 0
#define LWIP_STATS_DISPLAY 0
#define LWIP_DEBUG 0

// FreeRTOS threading
#define TCPIP_THREAD_STACKSIZE 4096  // bytes
#define DEFAULT_THREAD_STACKSIZE 2048
#define TCPIP_MBOX_SIZE 16
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#define DEFAULT_UDP_RECVMBOX_SIZE 8
#define DEFAULT_TCP_RECVMBOX_SIZE 8
#define DEFAULT_ACCEPTMBOX_SIZE 8  // browsers open several connections at once
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#ifdef CYW43_TASK_PRIORITY
// same as the CYW43 driver & other comms; lowest-priority tasks shouldn't stall the network
#define TCPIP_THREAD_PRIO CYW43_TASK_PRIORITY
#endif

// HACK: lwIP's FreeRTOS port assumes a tick is at least 1 ms. Our tick is
//       much shorter (`configTICK_RATE_HZ` is 20 kHz), so its integer
//       `portTICK_PERIOD_MS` is 0, which would make `sys_now()` always return 0
//       and its timeout conversions (`ms / portTICK_RATE_MS`) divide by zero.
//  -   `sys_now()` is ours instead, from the HW timer (see `wifi.cpp`).
//  -   `portTICK_RATE_MS` (pre-V8 name, only used by the port's ms -> tick
//      conversions) is defined as fractional ms per tick.
#define LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS 0
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS (1000.0f / configTICK_RATE_HZ)
#endif
