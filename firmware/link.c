/*
 * Link discovery + per-peer state machine.
 *
 * The implementation follows LINK_PROTOCOL_SPEC.md sections 5, 6, and 8.1.
 *
 * Time source: ghost_time.h's host_time_us() (1 MHz, 64-bit, hw counter).
 * Network:     net.h's tiny UDP sockets layered on LiteEth's UDP core.
 */

#include <string.h>
#include <stdlib.h>

#include "link.h"
#include "tlv.h"
#include "ghost_time.h"
#include "net.h"
#include "beat_pulse.h"
#include "config.h"

link_id_t        link_self_id;
link_id_t        link_self_session;
link_timeline_t  link_self_timeline;
link_startstop_t link_self_startstop;
link_peer_t      link_peers[LINK_MAX_PEERS];

static const uint8_t kProtoHdr[8] = { '_','a','s','d','p','_','v',0x01 };

static uint64_t s_last_bcast_us;     /* host time of last ALIVE we sent */
static uint64_t s_next_bcast_us;     /* host time at which to send next */
static int      s_state_dirty;       /* something changed since last ALIVE */

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

static uint8_t prng_byte(void) {
    /* Tiny LCG seeded from the host clock — good enough for NodeId. */
    static uint32_t s = 0;
    if (s == 0) s = (uint32_t)host_time_us() | 1;
    s = s * 1664525u + 1013904223u;
    return (uint8_t)(s >> 24);
}

void link_init(void) {
    for (int i = 0; i < 8; i++) {
        link_self_id.bytes[i] = prng_byte();
    }
    link_self_session = link_self_id;

    link_self_timeline.tempo_us_per_beat       = LINK_DEFAULT_TEMPO_US_PER_BEAT;
    link_self_timeline.beat_origin_microbeats  = 0;
    link_self_timeline.time_origin_us_ghost    = (int64_t)ghost_time_us();

    link_self_startstop.is_playing         = 0;
    link_self_startstop.beats_microbeats   = 0;
    link_self_startstop.timestamp_us_ghost = link_self_timeline.time_origin_us_ghost;

    memset(link_peers, 0, sizeof(link_peers));
    s_last_bcast_us = 0;
    s_next_bcast_us = host_time_us();
    s_state_dirty   = 1;

    /* Push initial timeline into BeatPulseGen so the GPIO outputs are
     * already running 120 BPM out of the box. */
    beat_pulse_load_timeline(&link_self_timeline,
                             &link_self_startstop);
}

/* ------------------------------------------------------------------ */
/* Encoders                                                           */
/* ------------------------------------------------------------------ */

static size_t encode_msg(uint8_t *buf, uint8_t mtype, int with_state) {
    uint8_t *p = buf;
    /* Protocol header (8) */
    memcpy(p, kProtoHdr, 8); p += 8;
    /* Message header (12): type, ttl, groupId(2), ident(8) */
    *p++ = mtype;
    *p++ = LINK_DISCOVERY_TTL_SEC;
    *p++ = 0; *p++ = 0;
    memcpy(p, link_self_id.bytes, 8); p += 8;

    if (!with_state) return p - buf;

    /* tmln */
    tlv_emit_header(&p, KEY_TMLN, 24);
    put_be64(p, (uint64_t)link_self_timeline.tempo_us_per_beat);          p += 8;
    put_be64(p, (uint64_t)link_self_timeline.beat_origin_microbeats);     p += 8;
    put_be64(p, (uint64_t)link_self_timeline.time_origin_us_ghost);       p += 8;

    /* sess */
    tlv_emit_bytes(&p, KEY_SESS, link_self_session.bytes, 8);

    /* stst */
    tlv_emit_header(&p, KEY_STST, 17);
    *p++ = link_self_startstop.is_playing ? 1 : 0;
    put_be64(p, (uint64_t)link_self_startstop.beats_microbeats);   p += 8;
    put_be64(p, (uint64_t)link_self_startstop.timestamp_us_ghost); p += 8;

    /* mep4 */
    tlv_emit_header(&p, KEY_MEP4, 6);
    put_be32(p, net_local_ipv4()); p += 4;
    put_be16(p, LINK_DISCOVERY_PORT); p += 2;

    /* mep6 — IPv6 measurement endpoint, link-local. */
    {
        const uint8_t *v6 = net_local_ipv6_bytes();
        tlv_emit_header(&p, KEY_MEP6, 18);
        memcpy(p, v6, 16); p += 16;
        put_be16(p, LINK_DISCOVERY_PORT); p += 2;
    }

#if LINK_NUM_TDM_PORTS > 0
    /* aep4 — audio endpoint on the same port; discriminated from
     * discovery traffic by the protocol header magic. */
    tlv_emit_header(&p, KEY_AEP4, 6);
    put_be32(p, net_local_ipv4()); p += 4;
    put_be16(p, LINK_DISCOVERY_PORT); p += 2;

    /* aep6 — same, IPv6. */
    {
        const uint8_t *v6 = net_local_ipv6_bytes();
        tlv_emit_header(&p, KEY_AEP6, 18);
        memcpy(p, v6, 16); p += 16;
        put_be16(p, LINK_DISCOVERY_PORT); p += 2;
    }
#endif

    return p - buf;
}

static void send_alive(void) {
    uint8_t buf[LINK_MAX_MSG_DISCOVERY];
    size_t  n = encode_msg(buf, /*ALIVE=*/1, 1);
    net_udp_send_v4_mcast(LINK_DISCOVERY_PORT,
                          LINK_DISCOVERY_PORT, buf, n);
    net_udp_send_v6_mcast(LINK_DISCOVERY_PORT,
                          LINK_DISCOVERY_PORT, buf, n);
    s_last_bcast_us = host_time_us();
    s_next_bcast_us = s_last_bcast_us + LINK_DISCOVERY_BCAST_MS * 1000ULL;
    s_state_dirty = 0;
}

static void send_response(uint32_t to_addr, uint16_t to_port) {
    uint8_t buf[LINK_MAX_MSG_DISCOVERY];
    size_t  n = encode_msg(buf, /*RESPONSE=*/2, 1);
    net_udp_send_v4_unicast(to_addr, to_port, LINK_DISCOVERY_PORT, buf, n);
}

void link_send_byebye(void) {
    uint8_t buf[LINK_MAX_MSG_DISCOVERY];
    uint8_t *p = buf;
    memcpy(p, kProtoHdr, 8); p += 8;
    *p++ = 3;          /* BYEBYE */
    *p++ = 0;          /* ttl=0 */
    *p++ = 0; *p++ = 0;
    memcpy(p, link_self_id.bytes, 8); p += 8;
    net_udp_send_v4_mcast(LINK_DISCOVERY_PORT,
                          LINK_DISCOVERY_PORT, buf, p - buf);
    net_udp_send_v6_mcast(LINK_DISCOVERY_PORT,
                          LINK_DISCOVERY_PORT, buf, p - buf);
}

void link_broadcast_alive_now(void) {
    s_state_dirty = 1;
    s_next_bcast_us = host_time_us();
}

/* ------------------------------------------------------------------ */
/* Decoder                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    link_peer_t *peer;
} parse_ctx_t;

static int handle_entry(uint32_t key, const uint8_t *v, uint32_t n, void *ctx_) {
    parse_ctx_t *ctx = (parse_ctx_t *)ctx_;
    link_peer_t *p   = ctx->peer;
    switch (key) {
    case KEY_TMLN:
        if (n != 24) return -1;
        p->timeline.tempo_us_per_beat       = (int64_t)be64(v);
        p->timeline.beat_origin_microbeats  = (int64_t)be64(v + 8);
        p->timeline.time_origin_us_ghost    = (int64_t)be64(v + 16);
        break;
    case KEY_SESS:
        if (n != 8) return -1;
        memcpy(p->session_id.bytes, v, 8);
        break;
    case KEY_STST:
        if (n != 17) return -1;
        p->startstop.is_playing         = v[0] ? 1 : 0;
        p->startstop.beats_microbeats   = (int64_t)be64(v + 1);
        p->startstop.timestamp_us_ghost = (int64_t)be64(v + 9);
        break;
    case KEY_MEP4:
        if (n != 6) return -1;
        p->mep4_addr = be32(v);
        p->mep4_port = be16(v + 4);
        break;
    case KEY_AEP4:
        if (n != 6) return -1;
        p->aep4_addr = be32(v);
        p->aep4_port = be16(v + 4);
        break;
    case KEY_MEP6:
        if (n != 18) return -1;
        memcpy(p->mep6_addr, v, 16);
        p->mep6_port = be16(v + 16);
        p->have_mep6 = 1;
        break;
    case KEY_AEP6:
        if (n != 18) return -1;
        memcpy(p->aep6_addr, v, 16);
        p->aep6_port = be16(v + 16);
        p->have_aep6 = 1;
        break;
    default: /* unknown — silently skip per spec §3.7 */
        break;
    }
    return 0;
}

static link_peer_t *find_or_alloc_peer(const link_id_t *id) {
    link_peer_t *free_slot = NULL;
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use && memcmp(p->node_id.bytes, id->bytes, 8) == 0)
            return p;
        if (!p->in_use && !free_slot) free_slot = p;
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->in_use = 1;
        free_slot->node_id = *id;
    }
    return free_slot;
}

static void evict_peer(const link_id_t *id) {
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use && memcmp(p->node_id.bytes, id->bytes, 8) == 0) {
            memset(p, 0, sizeof(*p));
            return;
        }
    }
}

void link_handle_rx(const uint8_t *buf, size_t len,
                    uint32_t src_addr, uint16_t src_port) {
    if (len < 8 + 12) return;
    if (memcmp(buf, kProtoHdr, 8) != 0) return;
    const uint8_t *p = buf + 8;
    uint8_t mtype  = *p++;
    uint8_t /*ttl=*/_t = *p++; (void)_t;
    uint16_t group = be16(p); p += 2;
    if (group != 0) return;
    link_id_t id;
    memcpy(id.bytes, p, 8); p += 8;

    /* Ignore our own broadcasts. */
    if (memcmp(id.bytes, link_self_id.bytes, 8) == 0) return;

    if (mtype == 3 /*BYEBYE*/) {
        evict_peer(&id);
        return;
    }
    if (mtype != 1 /*ALIVE*/ && mtype != 2 /*RESPONSE*/) return;

    link_peer_t *peer = find_or_alloc_peer(&id);
    if (!peer) return;          /* table full; drop */
    parse_ctx_t ctx = { .peer = peer };
    tlv_walk(p, len - (p - buf), handle_entry, &ctx);

    peer->expires_at_us = host_time_us()
                          + (uint64_t)LINK_DISCOVERY_TTL_SEC * 1000000ULL;

    /* If this is the first time we've seen this peer's session, kick a
     * measurement. (session.c keeps track of in-flight measurements.) */
    extern void session_observe_peer(link_peer_t *p);
    session_observe_peer(peer);

    if (mtype == 1 /*ALIVE*/) {
        send_response(src_addr, src_port);
    }
}

/* ------------------------------------------------------------------ */
/* Periodic tick                                                      */
/* ------------------------------------------------------------------ */

void link_tick(void) {
    uint64_t now = host_time_us();

    /* Broadcast schedule */
    if (s_state_dirty || now >= s_next_bcast_us) {
        if (now - s_last_bcast_us >= LINK_DISCOVERY_MIN_MS * 1000ULL) {
            send_alive();
        } else {
            /* Throttled — try again in min period */
            s_next_bcast_us = s_last_bcast_us
                              + LINK_DISCOVERY_MIN_MS * 1000ULL;
        }
    }

    /* Prune expired peers */
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use &&
            now > p->expires_at_us
                  + LINK_DISCOVERY_PRUNE_GRACE_MS * 1000ULL) {
            memset(p, 0, sizeof(*p));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Local edits                                                        */
/* ------------------------------------------------------------------ */

void link_set_local_tempo(double bpm) {
    int64_t us_per_beat = (int64_t)(60.0 * 1e6 / bpm + 0.5);
    int64_t now_g       = (int64_t)ghost_time_us();
    /* Re-anchor so the beat phase doesn't jump. */
    int64_t old_us = link_self_timeline.tempo_us_per_beat;
    int64_t old_t0 = link_self_timeline.time_origin_us_ghost;
    int64_t old_b0 = link_self_timeline.beat_origin_microbeats;
    int64_t cur_beats = old_b0
        + (int64_t)(((double)(now_g - old_t0) * 1e6) / (double)old_us);
    link_self_timeline.tempo_us_per_beat      = us_per_beat;
    link_self_timeline.beat_origin_microbeats = cur_beats;
    link_self_timeline.time_origin_us_ghost   = now_g;
    beat_pulse_load_timeline(&link_self_timeline, &link_self_startstop);
    link_broadcast_alive_now();
}

void link_set_play(int play) {
    int64_t now_g = (int64_t)ghost_time_us();
    /* Compute current beats so we can pin start/stop to it. */
    int64_t us_per_beat = link_self_timeline.tempo_us_per_beat;
    int64_t cur_beats   = link_self_timeline.beat_origin_microbeats
        + (int64_t)(((double)(now_g - link_self_timeline.time_origin_us_ghost) * 1e6)
                    / (double)us_per_beat);
    link_self_startstop.is_playing         = play ? 1 : 0;
    link_self_startstop.beats_microbeats   = cur_beats;
    link_self_startstop.timestamp_us_ghost = now_g;
    beat_pulse_load_timeline(&link_self_timeline, &link_self_startstop);
    link_broadcast_alive_now();
}
