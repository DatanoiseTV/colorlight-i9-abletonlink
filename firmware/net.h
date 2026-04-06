/*
 * LinkFPGA network layer — wraps lwIP's raw UDP API.
 *
 * Bring-up:
 *   net_init()              — bring up lwIP, create the netif, start DHCP,
 *                              join the IPv4 + IPv6 Link discovery groups.
 *   net_poll()              — must be called frequently from the main
 *                              loop. Drives the netif RX poll and lwIP's
 *                              periodic timers.
 *
 * Addresses:
 *   net_local_ipv4()        — current bound IPv4 (host byte order)
 *   net_local_ipv6_bytes()  — 16 bytes, link-local
 *   net_local_mac()         — 6 bytes
 *
 * UDP TX:
 *   net_udp_send_v4_unicast()  — single peer
 *   net_udp_send_v4_mcast()    — IPv4 multicast (224.76.78.75)
 *   net_udp_send_v6_mcast()    — IPv6 multicast (ff12::8080)
 *
 * UDP RX:
 *   net_set_rx_callback()      — invoked once per inbound UDP datagram
 *                                (any port). The dispatcher in main.c
 *                                routes by destination port.
 */

#ifndef LINKFPGA_NET_H
#define LINKFPGA_NET_H

#include <stddef.h>
#include <stdint.h>

void           net_init(void);
void           net_poll(void);

uint32_t       net_local_ipv4(void);            /* host byte order */
const uint8_t *net_local_ipv6_bytes(void);      /* 16 bytes link-local */
const uint8_t *net_local_mac(void);             /* 6 bytes */

int net_udp_send_v4_unicast(uint32_t dst_addr, uint16_t dst_port,
                            uint16_t src_port,
                            const void *buf, size_t len);
int net_udp_send_v4_mcast  (uint16_t dst_port,
                            uint16_t src_port,
                            const void *buf, size_t len);
int net_udp_send_v6_mcast  (uint16_t dst_port,
                            uint16_t src_port,
                            const void *buf, size_t len);

typedef void (*net_rx_cb_t)(uint32_t src_addr, uint16_t src_port,
                            uint16_t dst_port,
                            const uint8_t *buf, size_t len);
void net_set_rx_callback(net_rx_cb_t cb);

#endif /* LINKFPGA_NET_H */
