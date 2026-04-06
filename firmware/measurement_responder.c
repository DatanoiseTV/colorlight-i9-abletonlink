/*
 * Link v1 PingResponder side (LINK_PROTOCOL_SPEC.md §7.3, §8.2).
 *
 * Replies to incoming PINGs with a PONG carrying:
 *   - SessionMembership (sess) — our current session id
 *   - GHostTime (__gt)         — ghost-time at the moment we received
 *                                the PING (= host_time + intercept)
 *   - The original ping payload, echoed verbatim (per the reference
 *     implementation; in practice that means we echo __ht and any
 *     _pgt that was present).
 */

#include <string.h>

#include "link.h"
#include "tlv.h"
#include "net.h"
#include "ghost_time.h"
#include "config.h"

static const uint8_t kProtoLink[8] = { '_','l','i','n','k','_','v',0x01 };

void measurement_responder_handle_rx(const uint8_t *buf, size_t len,
                                     uint32_t src_addr, uint16_t src_port) {
    if (len < 8 + 1) return;
    if (memcmp(buf, kProtoLink, 8) != 0) return;
    if (buf[8] != 1 /*PING*/) return;

    /* The pong body is sess + __gt followed by the verbatim ping payload. */
    uint8_t out[LINK_MAX_MSG_LINK];
    uint8_t *p = out;
    memcpy(p, kProtoLink, 8); p += 8;
    *p++ = 2;       /* PONG */

    tlv_emit_bytes(&p, KEY_SESS, link_self_session.bytes, 8);
    tlv_emit_u64be(&p, KEY_GT, ghost_time_us());

    /* Echo the ping payload verbatim. */
    size_t ping_payload_len = len - 9;
    if (ping_payload_len > sizeof(out) - (size_t)(p - out)) return;
    memcpy(p, buf + 9, ping_payload_len);
    p += ping_payload_len;

    net_udp_send_v4_unicast(src_addr, src_port, LINK_DISCOVERY_PORT, out, p - out);
}
