/*
 * LinkFPGA network layer — wraps lwIP's raw UDP API.
 *
 * The netif lives in netif_litex.c (LiteEth driver). Here we bring up
 * lwIP's core, create UDP "raw" PCBs for unicast and multicast send,
 * subscribe to the Link discovery groups (224.76.78.75, ff12::8080),
 * and route every received UDP datagram through a single user
 * callback.
 *
 * The user callback receives both v4 and v6 packets through the same
 * entry point. The src_addr argument is host-order IPv4 for v4
 * packets and 0 for v6 packets (the upper layers don't use the source
 * address for IPv6 — Link's measurement endpoints are bound to v4
 * addresses anyway).
 */

#include <string.h>

#include <generated/csr.h>
#include <generated/soc.h>

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/mld6.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"

#include "net.h"
#include "ghost_time.h"
#include "config.h"

extern void          netif_litex_bringup(void);
extern void          netif_litex_input(void);
extern struct netif *netif_litex_get(void);

static net_rx_cb_t s_user_cb;

/* lwIP wants ms since boot — derive from our 1 MHz hardware counter.
 * The prototype comes from lwip/sys.h (`u32_t sys_now(void)`). */
u32_t sys_now(void) {
    return (u32_t)(host_time_us() / 1000ULL);
}

/* ---------------------------------------------------------------- */
/* RX dispatch                                                      */

static struct udp_pcb *s_rx_pcb;     /* receives all UDP, all ports */

static void udp_rx_handler(void *arg, struct udp_pcb *pcb,
                           struct pbuf *p,
                           const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    if (!p) return;

    if (s_user_cb && p->tot_len <= 1500) {
        uint8_t buf[1500];
        u16_t len = pbuf_copy_partial(p, buf, p->tot_len, 0);
        uint32_t v4 = 0;
        if (IP_IS_V4(addr)) v4 = lwip_htonl(ip4_addr_get_u32(ip_2_ip4(addr)));
        s_user_cb(v4, port, pcb->local_port, buf, len);
    }
    pbuf_free(p);
}

/* ---------------------------------------------------------------- */
/* Bring-up                                                         */

void net_init(void) {
    lwip_init();
    netif_litex_bringup();

    /* Receive PCB: bind to ANY:LINK_DISCOVERY_PORT to capture all
     * Link/Link-Audio/measurement traffic on the discovery port. We
     * also bind a second PCB on LINK_HTTP_PORT for the HTTP server,
     * but that's done by http_server.c on its own. */
    s_rx_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    udp_bind(s_rx_pcb, IP_ANY_TYPE, LINK_DISCOVERY_PORT);
    udp_recv(s_rx_pcb, udp_rx_handler, NULL);

    /* Join the IPv4 multicast group. */
    {
        ip4_addr_t group;
        IP4_ADDR(&group, 224, 76, 78, 75);
        igmp_joingroup(IP4_ADDR_ANY4, &group);
    }
    /* Join the IPv6 multicast group ff12::8080 on the netif. */
    {
        ip6_addr_t group6;
        IP6_ADDR(&group6, PP_HTONL(0xff120000), 0, 0, PP_HTONL(0x00008080));
        mld6_joingroup_netif(netif_litex_get(), &group6);
    }
}

void net_poll(void) {
    netif_litex_input();
    sys_check_timeouts();
}

uint32_t net_local_ipv4(void) {
    struct netif *n = netif_litex_get();
    return lwip_htonl(ip4_addr_get_u32(netif_ip4_addr(n)));
}

const uint8_t *net_local_ipv6_bytes(void) {
    struct netif *n = netif_litex_get();
    return (const uint8_t *)netif_ip6_addr(n, 0)->addr;
}

const uint8_t *net_local_mac(void) {
    return netif_litex_get()->hwaddr;
}

void net_set_rx_callback(net_rx_cb_t cb) { s_user_cb = cb; }

/* ---------------------------------------------------------------- */
/* TX                                                               */

static int udp_tx(const ip_addr_t *dst, uint16_t dst_port,
                  uint16_t src_port,
                  const void *buf, size_t len) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return -1;
    memcpy(p->payload, buf, len);

    /* Allocate a one-shot PCB for this transmission so we can set the
     * source port without disturbing the rx PCB. */
    struct udp_pcb *tx = udp_new_ip_type(IP_GET_TYPE(dst));
    if (!tx) { pbuf_free(p); return -2; }
    udp_bind(tx, IP_ANY_TYPE, src_port);
    err_t err = udp_sendto(tx, p, dst, dst_port);
    udp_remove(tx);
    pbuf_free(p);
    return err == ERR_OK ? 0 : -3;
}

int net_udp_send_v4_unicast(uint32_t dst_addr, uint16_t dst_port,
                            uint16_t src_port,
                            const void *buf, size_t len) {
    ip_addr_t dst;
    IP_ADDR4(&dst, (dst_addr >> 24) & 0xff,
                   (dst_addr >> 16) & 0xff,
                   (dst_addr >>  8) & 0xff,
                    dst_addr        & 0xff);
    return udp_tx(&dst, dst_port, src_port, buf, len);
}

int net_udp_send_v4_mcast(uint16_t dst_port, uint16_t src_port,
                          const void *buf, size_t len) {
    ip_addr_t dst;
    IP_ADDR4(&dst, 224, 76, 78, 75);
    return udp_tx(&dst, dst_port, src_port, buf, len);
}

int net_udp_send_v6_mcast(uint16_t dst_port, uint16_t src_port,
                          const void *buf, size_t len) {
    ip_addr_t dst;
    IP_ADDR6(&dst, PP_HTONL(0xff120000), 0, 0, PP_HTONL(0x00008080));
    return udp_tx(&dst, dst_port, src_port, buf, len);
}
