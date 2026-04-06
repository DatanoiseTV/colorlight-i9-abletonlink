/*
 * lwIP configuration for the LinkFPGA firmware on VexRiscv.
 *
 * Targets:
 *   - bare metal (NO_SYS=1, no RTOS, no threads)
 *   - IPv4 + IPv6 dual stack
 *   - UDP raw API (Link discovery, ping/pong, Link-Audio)
 *   - TCP raw API (HTTP admin server)
 *   - IGMP for IPv4 multicast (224.76.78.75)
 *   - MLD for IPv6 link-local multicast (ff12::8080)
 *
 * Memory budget: ~64 KiB heap + 16 KiB pbuf pool. The board has 32 MB
 * SDRAM so we have plenty of room.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ---------- platform / threading ------------------------------- */
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define LWIP_TIMERS_CUSTOM              0
/* Single-threaded polling main loop — no need for protect/unprotect. */
#define SYS_LIGHTWEIGHT_PROT            0

/* ---------- core protocols ------------------------------------- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_ICMP                       1
#define LWIP_ICMP6                      1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     1
#define LWIP_DHCP_AUTOIP_COOP           1
#define LWIP_DHCP_AUTOIP_COOP_TRIES     2
#define LWIP_DNS                        0
#define LWIP_RAW                        1
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_STATS                      0
#define LWIP_HAVE_LOOPIF                0

/* ---------- multicast ------------------------------------------ */
#define LWIP_IGMP                       1
#define LWIP_IPV6_MLD                   1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_IPV6_DHCP6                 0
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   1

/* ---------- memory --------------------------------------------- */
/* The .data + .bss for the BIOS, lwIP, and our firmware all live in
 * the integrated SRAM region (32 KiB on this build). Trim every lwIP
 * static array to the bare minimum: this is a low-traffic studio LAN
 * appliance, not a server, so the pools are tiny. */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (8 * 1024)
#define MEMP_NUM_PBUF                   8
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         1
#define MEMP_NUM_TCP_SEG                8
#define MEMP_NUM_REASSDATA              2
#define MEMP_NUM_FRAG_PBUF              4
#define MEMP_NUM_NETBUF                 0
#define MEMP_NUM_NETCONN                0
#define MEMP_NUM_TCPIP_MSG_API          0
#define MEMP_NUM_TCPIP_MSG_INPKT        0
#define MEMP_NUM_SYS_TIMEOUT            16
#define PBUF_POOL_SIZE                  4
#define PBUF_POOL_BUFSIZE               1536

/* ---------- TCP ------------------------------------------------ */
#define TCP_MSS                         536
#define TCP_SND_BUF                     (2 * TCP_MSS)
#define TCP_WND                         (2 * TCP_MSS)
#define TCP_LISTEN_BACKLOG              1
#define LWIP_TCP_KEEPALIVE              0

/* ---------- UDP ------------------------------------------------ */
#define LWIP_UDPLITE                    0

/* ---------- ARP / ND / etc ------------------------------------- */
#define LWIP_ARP                        1
#define ARP_TABLE_SIZE                  10
#define ARP_QUEUEING                    0
#define ETHARP_SUPPORT_VLAN             0

/* ---------- checksums ------------------------------------------ */
/* Let lwIP do all checksums in software. The LiteEth MAC has no
 * checksum offload exposed via libliteeth. */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

/* ---------- debug ---------------------------------------------- */
#define LWIP_DEBUG                      0
#define LWIP_NOASSERT                   1

/* `sys_now()` (returns ms since boot) is declared in `lwip/sys.h`
 * and implemented in `net.c`. We don't redeclare it here. */

#endif /* LWIPOPTS_H */
