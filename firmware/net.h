/*
 * Tiny UDP wrapper around LiteEth's UDP core.
 *
 * LinkFPGA only ever talks UDP, so we don't drag in a full TCP/IP stack.
 * The functions here are blocking; they assume the caller is the only
 * one driving the LiteEth UDP datapath (which it is, since the firmware
 * is single-threaded and event-driven).
 */

#ifndef LINKFPGA_NET_H
#define LINKFPGA_NET_H

#include <stddef.h>
#include <stdint.h>

void     net_init(void);
void     net_poll(void);                       /* call from main loop */

uint32_t net_local_ipv4(void);                 /* network byte order */
const uint8_t *net_local_mac(void);            /* 6 bytes */

void     net_udp_bind(uint16_t port);
int      net_udp_send_to(uint32_t addr, uint16_t port,
                         const void *buf, size_t len);
int      net_udp_send_mcast(uint16_t port, const void *buf, size_t len);

/* Caller-supplied dispatch — invoked once per received UDP datagram. */
typedef void (*net_rx_cb_t)(uint32_t src_addr, uint16_t src_port,
                            uint16_t dst_port,
                            const uint8_t *buf, size_t len);
void     net_set_rx_callback(net_rx_cb_t cb);

#endif /* LINKFPGA_NET_H */
