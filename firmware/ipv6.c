/*
 * Bare-minimum IPv6 link-local multicast over LiteEth raw frames.
 * See ipv6.h for scope notes.
 *
 * Frame layout we emit (and parse on receive):
 *
 *   Ethernet (14):
 *     dst MAC  6   33:33:xx:xx:xx:xx (mapped from mcast addr)
 *     src MAC  6   our local MAC
 *     ethtype  2   0x86dd
 *
 *   IPv6 (40):
 *     vtcfl    4   0x60000000          (version=6, tc=0, fl=0)
 *     plen     2   payload length (UDP+data)
 *     nh       1   17 (UDP)
 *     hlim     1   1   (link-local)
 *     src      16  fe80::EUI64
 *     dst      16  multicast group
 *
 *   UDP (8):
 *     sport    2
 *     dport    2
 *     ulen     2   8 + datalen
 *     csum     2   IPv6 pseudo-header checksum (mandatory in v6)
 *
 *   data     N
 */

#include <string.h>

#include "ipv6.h"
#include "config.h"

extern const uint8_t *liteeth_get_mac(void);
extern int  liteeth_raw_send(const void *frame, size_t len);
extern void liteeth_raw_set_callback(
                void (*cb)(const void *frame, size_t len));

const uint8_t IPV6_LINK_MCAST[IPV6_ADDR_LEN] = {
    0xff,0x12, 0,0, 0,0,0,0, 0,0,0,0, 0,0, 0x80,0x80
};

static uint8_t      s_local_addr[IPV6_ADDR_LEN];
static ipv6_rx_cb_t s_user_cb;

static uint16_t csum16(const uint8_t *buf, size_t len, uint32_t init) {
    uint32_t s = init;
    for (size_t i = 0; i + 1 < len; i += 2) s += ((uint32_t)buf[i] << 8) | buf[i+1];
    if (len & 1) s += (uint32_t)buf[len-1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s & 0xffff;
}

static uint16_t udp6_checksum(const uint8_t *src, const uint8_t *dst,
                              const uint8_t *udp, size_t udp_len) {
    /* Pseudo-header sum */
    uint32_t s = 0;
    for (int i = 0; i < 16; i += 2) s += ((uint32_t)src[i] << 8) | src[i+1];
    for (int i = 0; i < 16; i += 2) s += ((uint32_t)dst[i] << 8) | dst[i+1];
    s += (uint32_t)udp_len;
    s += 17;        /* next header = UDP */
    return csum16(udp, udp_len, s);
}

static void make_link_local_eui64(uint8_t out[16], const uint8_t mac[6]) {
    /* fe80::ffff:fe.. EUI-64 derivation */
    out[0]  = 0xfe; out[1]  = 0x80;
    for (int i = 2; i < 8; i++) out[i] = 0;
    out[8]  = mac[0] ^ 0x02;     /* flip U/L */
    out[9]  = mac[1];
    out[10] = mac[2];
    out[11] = 0xff;
    out[12] = 0xfe;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

static void on_raw_frame(const void *fbuf, size_t flen) {
    const uint8_t *f = (const uint8_t *)fbuf;
    if (flen < 14 + 40 + 8) return;
    if (f[12] != 0x86 || f[13] != 0xdd) return;       /* not IPv6 */
    const uint8_t *ip6 = f + 14;
    if ((ip6[0] >> 4) != 6) return;
    if (ip6[6] != 17) return;                         /* not UDP */
    /* Only accept multicast destined to our group (or our local addr). */
    if (memcmp(ip6 + 24, IPV6_LINK_MCAST, IPV6_ADDR_LEN) != 0 &&
        memcmp(ip6 + 24, s_local_addr,    IPV6_ADDR_LEN) != 0) return;
    const uint8_t *udp = ip6 + 40;
    uint16_t sp = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t dp = ((uint16_t)udp[2] << 8) | udp[3];
    uint16_t ul = ((uint16_t)udp[4] << 8) | udp[5];
    if (ul < 8 || ul > flen - 14 - 40) return;
    if (s_user_cb)
        s_user_cb(ip6 + 8, sp, dp, udp + 8, ul - 8);
}

void ipv6_init(void) {
    make_link_local_eui64(s_local_addr, liteeth_get_mac());
    liteeth_raw_set_callback(on_raw_frame);
}

const uint8_t *ipv6_local_addr(void) { return s_local_addr; }

void ipv6_set_rx_callback(ipv6_rx_cb_t cb) { s_user_cb = cb; }

int ipv6_send_mcast(uint16_t dst_port, const void *data, size_t data_len) {
    if (data_len > 1500 - 14 - 40 - 8) return -1;
    uint8_t frame[1600];
    uint8_t *p = frame;

    /* Ethernet */
    p[0] = 0x33; p[1] = 0x33;
    p[2] = IPV6_LINK_MCAST[12]; p[3] = IPV6_LINK_MCAST[13];
    p[4] = IPV6_LINK_MCAST[14]; p[5] = IPV6_LINK_MCAST[15];
    memcpy(p + 6, liteeth_get_mac(), 6);
    p[12] = 0x86; p[13] = 0xdd;
    p += 14;

    /* IPv6 */
    p[0] = 0x60; p[1] = 0; p[2] = 0; p[3] = 0;
    uint16_t plen = 8 + data_len;
    p[4] = plen >> 8; p[5] = plen & 0xff;
    p[6] = 17;          /* UDP */
    p[7] = 1;           /* hop limit */
    memcpy(p + 8,  s_local_addr,    16);
    memcpy(p + 24, IPV6_LINK_MCAST, 16);
    p += 40;

    /* UDP */
    uint8_t *udp = p;
    udp[0] = LINK_DISCOVERY_PORT >> 8; udp[1] = LINK_DISCOVERY_PORT & 0xff;
    udp[2] = dst_port >> 8;            udp[3] = dst_port & 0xff;
    udp[4] = plen >> 8;                udp[5] = plen & 0xff;
    udp[6] = 0;                        udp[7] = 0;          /* csum, fill below */
    memcpy(udp + 8, data, data_len);

    uint16_t csum = udp6_checksum(s_local_addr, IPV6_LINK_MCAST, udp, plen);
    if (csum == 0) csum = 0xffff;     /* RFC 2460 §8.1 */
    udp[6] = csum >> 8;
    udp[7] = csum & 0xff;
    p += plen;

    return liteeth_raw_send(frame, p - frame);
}
