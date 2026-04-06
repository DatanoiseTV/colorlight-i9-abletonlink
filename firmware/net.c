/*
 * Minimal LiteEth UDP wrapper.
 *
 * LiteX exposes a "raw socket" style API on top of LiteEthMAC; we use it
 * directly to keep the firmware footprint small. The protocol-spec layer
 * does its own multicast group join because LiteEth's stock IP core does
 * not bind to multicast addresses by default — instead we promiscuously
 * accept all UDP frames and demux in software.
 */

#include <string.h>
#include <stdio.h>

#include "net.h"
#include "config.h"

/* Forward decls into LiteEth's libliteeth — these are provided by the
 * generated SoC build, not by us. The exact symbols depend on the LiteEth
 * version; the names below match the current `liteethmac` package. */
extern void  liteeth_init(void);
extern void  liteeth_poll(void);
extern int   liteeth_udp_send(uint32_t dst_ip, uint16_t dst_port,
                              uint16_t src_port,
                              const void *data, size_t len);
extern void  liteeth_set_rx_callback(
                 void (*cb)(uint32_t src_ip, uint16_t src_port,
                            uint16_t dst_port,
                            const void *data, size_t len));
extern uint32_t liteeth_get_ip(void);
extern const uint8_t *liteeth_get_mac(void);

static net_rx_cb_t s_user_cb;

static void rx_trampoline(uint32_t src_ip, uint16_t src_port,
                          uint16_t dst_port,
                          const void *data, size_t len) {
    if (s_user_cb)
        s_user_cb(src_ip, src_port, dst_port, (const uint8_t *)data, len);
}

void net_init(void) {
    liteeth_init();
    liteeth_set_rx_callback(rx_trampoline);
}

void net_poll(void) {
    liteeth_poll();
}

uint32_t net_local_ipv4(void) {
    return liteeth_get_ip();
}

const uint8_t *net_local_mac(void) {
    return liteeth_get_mac();
}

void net_udp_bind(uint16_t port) {
    /* No-op: we accept all ports and dispatch in software. */
    (void)port;
}

int net_udp_send_to(uint32_t addr, uint16_t port,
                    const void *buf, size_t len) {
    return liteeth_udp_send(addr, port, LINK_DISCOVERY_PORT, buf, len);
}

int net_udp_send_mcast(uint16_t port, const void *buf, size_t len) {
    /* 224.76.78.75 in network byte order */
    uint32_t mcast = (224u << 24) | (76u << 16) | (78u << 8) | 75u;
    return liteeth_udp_send(mcast, port, LINK_DISCOVERY_PORT, buf, len);
}

void net_set_rx_callback(net_rx_cb_t cb) {
    s_user_cb = cb;
}
