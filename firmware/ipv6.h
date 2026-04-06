/*
 * Minimal IPv6 link-local multicast support, sufficient for the Ableton
 * Link discovery protocol described in §4.1 of LINK_PROTOCOL_SPEC.md.
 *
 * Scope of this implementation:
 *   - Construct and send link-local IPv6 UDP datagrams to a multicast
 *     destination (default ff12::8080) at a configurable port.
 *   - Receive UDP datagrams arriving on the same multicast group.
 *   - Maintain a single auto-derived link-local source address
 *     (fe80::/64 + EUI-64 from the LiteEth MAC).
 *
 * Out of scope (and explicitly OK to skip per the spec, since the
 * audio extension is unicast and discovery is link-local):
 *   - Neighbor Discovery (we hard-derive the destination MAC from the
 *     IPv6 multicast address: 33:33:xx:xx:xx:xx where xx is the lower
 *     32 bits of the IPv6 address).
 *   - Path MTU discovery (datagrams are bounded by LINK_MAX_MSG_*).
 *   - Router advertisement (we never leave the link).
 *
 * The implementation talks directly to LiteEth's raw Ethernet frame
 * interface (`liteeth_raw_send` / `liteeth_raw_set_callback`), which is
 * the only LiteEth API exposed for non-IPv4 traffic. We do *not* use
 * LiteEth's UDP core for IPv6.
 */

#ifndef LINKFPGA_IPV6_H
#define LINKFPGA_IPV6_H

#include <stddef.h>
#include <stdint.h>

#define IPV6_ADDR_LEN 16

/* The multicast group we always join: ff12::8080 (link-local, transient
 * multicast, 16-bit lower group "0x8080"). */
extern const uint8_t IPV6_LINK_MCAST[IPV6_ADDR_LEN];

void     ipv6_init(void);
int      ipv6_send_mcast(uint16_t dst_port, const void *buf, size_t len);
const uint8_t *ipv6_local_addr(void);   /* 16 bytes link-local */

/* Caller-supplied dispatch — invoked once per received UDP/IPv6
 * datagram destined to our group. */
typedef void (*ipv6_rx_cb_t)(const uint8_t src_addr[IPV6_ADDR_LEN],
                             uint16_t src_port,
                             uint16_t dst_port,
                             const uint8_t *buf, size_t len);
void     ipv6_set_rx_callback(ipv6_rx_cb_t cb);

#endif /* LINKFPGA_IPV6_H */
