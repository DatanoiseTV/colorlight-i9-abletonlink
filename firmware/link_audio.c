/*
 * Link-Audio extension (LINK_PROTOCOL_SPEC.md §9).
 *
 * What this implements:
 *
 *   - Periodic kPeerAnnouncement (type 1) to every audio peer we know
 *     about. Carries:
 *         sess  — current session id
 *         __pi  — our human-readable name
 *         auca  — vector of <name, channel_id> we publish
 *         __ht  — embedded ping (one per round per receiver, §9.6)
 *
 *   - kPong (type 3) replies to incoming embedded pings, and RTT
 *     bookkeeping when we receive a Pong of our own.
 *
 *   - kChannelRequest (type 4) and kStopChannelRequest (type 5)
 *     encode + receive: full subscription state machine. The
 *     re-request schedule is driven by a per-subscription timer
 *     (§9.7: "the requesting peer re-sends every kTtl seconds").
 *
 *   - kChannelByes (type 2) encode (on shutdown / channel removal) +
 *     receive (clear our remote-channel catalog entries).
 *
 *   - kAudioBuffer (type 6) encode and decode, raw layout per §9.8
 *     (NOT TLV-wrapped). Multi-channel decode supported (interleaved
 *     i16 BE), de-interleaved into separate jitter rings as the
 *     subscription model only routes one channel at a time.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "link_audio.h"
#include "link.h"
#include "tlv.h"
#include "net.h"
#include "ghost_time.h"
#include "tdm.h"
#include "config.h"

static const uint8_t kProtoAudio[8] = { 'c','h','n','n','l','s','v',0x01 };

#define MAX_LOCAL_CHANNELS  LINK_MAX_CHANNELS
#define AUDIO_FRAMES_PER_PKT 64
#define AUDIO_REQUEST_TTL_MS 5000     /* §9.7 — kTtl = 5 s */

link_audio_local_t  link_audio_local [MAX_LOCAL_CHANNELS];
link_audio_remote_t link_audio_remote[LINK_AUDIO_MAX_REMOTE];
link_audio_sub_t    link_audio_subs  [LINK_AUDIO_MAX_REMOTE];

static char  s_peer_name[LINK_AUDIO_NAME_MAX] = "linkfpga";
static uint64_t s_last_announce_us;
static uint16_t s_tx_pkt_buf[LINK_NUM_TDM_PORTS]
                            [LINK_TDM_CHANNELS_PER_PORT]
                            [AUDIO_FRAMES_PER_PKT];
static uint32_t s_tx_pkt_pos[LINK_NUM_TDM_PORTS]
                            [LINK_TDM_CHANNELS_PER_PORT];

/* ---------------------------------------------------------------- */
/* Initialisation                                                   */
/* ---------------------------------------------------------------- */

static void random_id(link_id_t *out) {
    uint64_t t = host_time_us();
    for (int i = 0; i < 8; i++) {
        out->bytes[i] = (uint8_t)(t >> (i * 8)) ^ (uint8_t)(0xa5 + i * 7);
    }
}

void link_audio_init(void) {
    memset(link_audio_local,  0, sizeof(link_audio_local));
    memset(link_audio_remote, 0, sizeof(link_audio_remote));
    memset(link_audio_subs,   0, sizeof(link_audio_subs));

    int idx = 0;
    for (int port = 0; port < LINK_NUM_TDM_PORTS; port++) {
        for (int ch = 0; ch < LINK_TDM_CHANNELS_PER_PORT
                       && idx < MAX_LOCAL_CHANNELS; ch++) {
            link_audio_local_t *c = &link_audio_local[idx];
            c->in_use      = 1;
            c->tdm_port    = port;
            c->tdm_channel = ch;
            random_id(&c->channel_id);
            snprintf(c->name, LINK_AUDIO_NAME_MAX, "tdm%d-ch%02d", port, ch);
            idx++;
        }
    }
    s_last_announce_us = 0;
}

/* ---------------------------------------------------------------- */
/* Encoder helpers                                                  */
/* ---------------------------------------------------------------- */

static size_t encode_audio_header(uint8_t *p, uint8_t mtype, uint8_t ttl) {
    uint8_t *q = p;
    memcpy(q, kProtoAudio, 8); q += 8;
    *q++ = mtype;
    *q++ = ttl;
    *q++ = 0; *q++ = 0;        /* groupId */
    memcpy(q, link_self_id.bytes, 8); q += 8;
    return q - p;
}

/* ---------------------------------------------------------------- */
/* PeerAnnouncement encode + send                                   */
/* ---------------------------------------------------------------- */

static size_t encode_peer_announcement(uint8_t *buf, int with_ping) {
    uint8_t *p = buf;
    p += encode_audio_header(p, /*kPeerAnnouncement=*/1, LINK_DISCOVERY_TTL_SEC);

    /* sess */
    tlv_emit_bytes(&p, KEY_SESS, link_self_session.bytes, 8);

    /* __pi (string) */
    {
        uint32_t name_len = (uint32_t)strlen(s_peer_name);
        tlv_emit_header(&p, KEY_PI, 4 + name_len);
        put_be32(p, name_len); p += 4;
        memcpy(p, s_peer_name, name_len); p += name_len;
    }

    /* auca: vector<ChannelAnnouncement>
     *       ChannelAnnouncement = string name + 8-byte id              */
    {
        uint32_t total = 4;     /* vector count */
        uint32_t count = 0;
        for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
            if (!link_audio_local[i].in_use) continue;
            uint32_t name_len = (uint32_t)strlen(link_audio_local[i].name);
            total += 4 + name_len + 8;
            count++;
        }
        tlv_emit_header(&p, KEY_AUCA, total);
        put_be32(p, count); p += 4;
        for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
            link_audio_local_t *c = &link_audio_local[i];
            if (!c->in_use) continue;
            uint32_t name_len = (uint32_t)strlen(c->name);
            put_be32(p, name_len); p += 4;
            memcpy(p, c->name, name_len); p += name_len;
            memcpy(p, c->channel_id.bytes, 8); p += 8;
        }
    }

    if (with_ping) {
        tlv_emit_u64be(&p, KEY_HT, host_time_us());
    }

    return p - buf;
}

static void send_announcements(void) {
    uint8_t buf[LINK_MAX_MSG_AUDIO];
    int first = 1;
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *peer = &link_peers[i];
        if (!peer->in_use || peer->aep4_addr == 0) continue;
        size_t n = encode_peer_announcement(buf, first);
        net_udp_send_v4_unicast(peer->aep4_addr, peer->aep4_port,
                             LINK_DISCOVERY_PORT, buf, n);
        first = 0;
    }
    s_last_announce_us = host_time_us();
}

/* ---------------------------------------------------------------- */
/* Pong                                                              */
/* ---------------------------------------------------------------- */

static void send_pong(uint32_t addr, uint16_t port, int64_t echoed_ht) {
    uint8_t buf[LINK_MAX_MSG_AUDIO];
    uint8_t *p = buf;
    p += encode_audio_header(p, /*kPong=*/3, LINK_DISCOVERY_TTL_SEC);
    tlv_emit_u64be(&p, KEY_HT, (uint64_t)echoed_ht);
    net_udp_send_v4_unicast(addr, port, LINK_DISCOVERY_PORT, buf, p - buf);
}

/* ---------------------------------------------------------------- */
/* ChannelRequest / ChannelStopRequest                              */
/* ---------------------------------------------------------------- */

static void send_channel_request(uint8_t mtype, uint32_t addr, uint16_t port,
                                 const link_id_t *channel_id) {
    uint8_t buf[LINK_MAX_MSG_AUDIO];
    uint8_t *p = buf;
    p += encode_audio_header(p, mtype, /*ttl=*/AUDIO_REQUEST_TTL_MS / 1000);
    tlv_emit_bytes(&p, KEY_CHID, channel_id->bytes, 8);
    net_udp_send_v4_unicast(addr, port, LINK_DISCOVERY_PORT, buf, p - buf);
}

static link_audio_local_t *find_local_channel(const link_id_t *cid) {
    for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
        if (link_audio_local[i].in_use &&
            memcmp(link_audio_local[i].channel_id.bytes, cid->bytes, 8) == 0)
            return &link_audio_local[i];
    }
    return NULL;
}

static int handle_chid_entry(uint32_t key, const uint8_t *v, uint32_t n,
                             void *ctx) {
    link_id_t *out = (link_id_t *)ctx;
    if (key == KEY_CHID && n == 8) memcpy(out->bytes, v, 8);
    return 0;
}

static void add_subscriber(link_audio_local_t *c,
                           const link_id_t *peer_id,
                           uint32_t addr, uint16_t port) {
    /* Already subscribed? */
    for (int i = 0; i < c->num_subscribers; i++) {
        if (c->sub_addr[i] == addr && c->sub_port[i] == port)
            return;
    }
    if (c->num_subscribers >= LINK_AUDIO_MAX_SUB_PER) return;
    int i = c->num_subscribers++;
    c->sub_addr[i] = addr;
    c->sub_port[i] = port;
    c->sub_peer[i] = *peer_id;
}

static void remove_subscriber(link_audio_local_t *c,
                              uint32_t addr, uint16_t port) {
    for (int i = 0; i < c->num_subscribers; i++) {
        if (c->sub_addr[i] == addr && c->sub_port[i] == port) {
            c->num_subscribers--;
            for (int j = i; j < c->num_subscribers; j++) {
                c->sub_addr[j] = c->sub_addr[j + 1];
                c->sub_port[j] = c->sub_port[j + 1];
                c->sub_peer[j] = c->sub_peer[j + 1];
            }
            return;
        }
    }
}

static void handle_channel_request(uint8_t mtype,
                                   const link_id_t *requester,
                                   uint32_t src_addr, uint16_t src_port,
                                   const uint8_t *body, size_t bodylen) {
    link_id_t cid = {{0}};
    tlv_walk(body, bodylen, handle_chid_entry, &cid);
    link_audio_local_t *c = find_local_channel(&cid);
    if (!c) return;
    if (mtype == 4 /*kChannelRequest*/) {
        add_subscriber(c, requester, src_addr, src_port);
    } else { /* kStopChannelRequest */
        remove_subscriber(c, src_addr, src_port);
    }
}

/* ---------------------------------------------------------------- */
/* ChannelByes encode / decode                                      */
/* ---------------------------------------------------------------- */

static int handle_aucb_entry(uint32_t key, const uint8_t *v, uint32_t n,
                             void *ctx) {
    (void)ctx;
    if (key != KEY_AUCB) return 0;
    if (n < 4) return -1;
    uint32_t count = be32(v); v += 4; n -= 4;
    if (n < count * 8) return -1;
    for (uint32_t i = 0; i < count; i++) {
        link_id_t cid;
        memcpy(cid.bytes, v + i * 8, 8);
        /* Drop any remote-catalog entries with this channel id, drop any
         * subscriptions to it, drop any peer-side subscribers (the
         * remote peer just told us they're going away). */
        for (int j = 0; j < LINK_AUDIO_MAX_REMOTE; j++) {
            if (link_audio_remote[j].in_use &&
                memcmp(link_audio_remote[j].channel_id.bytes,
                       cid.bytes, 8) == 0) {
                link_audio_remote[j].in_use = 0;
            }
            if (link_audio_subs[j].in_use &&
                memcmp(link_audio_subs[j].channel_id.bytes,
                       cid.bytes, 8) == 0) {
                link_audio_subs[j].in_use = 0;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* PeerAnnouncement decode (extracts session, name, channels, ping) */
/* ---------------------------------------------------------------- */

typedef struct {
    int64_t  ping_ht;
    int      have_ping;
    link_id_t peer_id;
    uint32_t peer_addr;
    uint16_t peer_port;
    char     peer_name[LINK_AUDIO_NAME_MAX];
} ann_ctx_t;

static int ann_handler(uint32_t key, const uint8_t *v, uint32_t n,
                       void *ctx_) {
    ann_ctx_t *ctx = (ann_ctx_t *)ctx_;
    switch (key) {
    case KEY_HT:
        if (n != 8) return -1;
        ctx->ping_ht = (int64_t)be64(v);
        ctx->have_ping = 1;
        break;
    case KEY_PI: {
        if (n < 4) return -1;
        uint32_t len = be32(v);
        if (n < 4 + len) return -1;
        if (len >= LINK_AUDIO_NAME_MAX) len = LINK_AUDIO_NAME_MAX - 1;
        memcpy(ctx->peer_name, v + 4, len);
        ctx->peer_name[len] = 0;
        break;
    }
    case KEY_SESS:
        if (n != 8) return -1;
        /* Session is enforced at the AudioBuffer layer; we don't act
         * on it here. */
        break;
    case KEY_AUCA: {
        if (n < 4) return -1;
        uint32_t count = be32(v); v += 4; n -= 4;
        for (uint32_t i = 0; i < count; i++) {
            if (n < 4) return -1;
            uint32_t name_len = be32(v); v += 4; n -= 4;
            if (n < name_len + 8) return -1;
            char name[LINK_AUDIO_NAME_MAX] = {0};
            uint32_t cp = name_len < LINK_AUDIO_NAME_MAX
                              ? name_len : LINK_AUDIO_NAME_MAX - 1;
            memcpy(name, v, cp);
            v += name_len; n -= name_len;
            link_id_t cid;
            memcpy(cid.bytes, v, 8); v += 8; n -= 8;

            /* Find existing or allocate new remote-catalog slot. */
            link_audio_remote_t *r = NULL;
            int free_slot = -1;
            for (int s = 0; s < LINK_AUDIO_MAX_REMOTE; s++) {
                if (link_audio_remote[s].in_use &&
                    memcmp(link_audio_remote[s].channel_id.bytes,
                           cid.bytes, 8) == 0) {
                    r = &link_audio_remote[s];
                    break;
                }
                if (!link_audio_remote[s].in_use && free_slot < 0)
                    free_slot = s;
            }
            if (!r) {
                if (free_slot < 0) continue;
                r = &link_audio_remote[free_slot];
                r->in_use = 1;
                r->peer_id = ctx->peer_id;
                r->channel_id = cid;
            }
            memcpy(r->name, name, sizeof(r->name));
            r->endpoint_addr = ctx->peer_addr;
            r->endpoint_port = ctx->peer_port;
            r->last_seen_us  = host_time_us();
        }
        break;
    }
    default: break;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* AudioBuffer encode + send                                        */
/* ---------------------------------------------------------------- */

static void send_audio_buffer_for(link_audio_local_t *c, uint16_t *samples) {
    if (c->num_subscribers == 0) return;

    int64_t now_g  = (int64_t)ghost_time_us();
    int64_t span_us= (int64_t)AUDIO_FRAMES_PER_PKT * 1000000LL / LINK_TDM_FS_HZ;
    int64_t pkt_t0 = now_g - span_us;
    int64_t bp     = link_self_timeline.tempo_us_per_beat;
    int64_t beats_first = link_self_timeline.beat_origin_microbeats
        + (int64_t)(((double)(pkt_t0 - link_self_timeline.time_origin_us_ghost)
                     * 1e6) / (double)bp);

    uint8_t buf[LINK_MAX_MSG_AUDIO];
    uint8_t *p = buf;
    p += encode_audio_header(p, /*kAudioBuffer=*/6, /*ttl=*/0);

    /* Raw AudioBuffer body (NOT TLV) */
    memcpy(p, c->channel_id.bytes,    8); p += 8;
    memcpy(p, link_self_session.bytes, 8); p += 8;
    put_be32(p, 1); p += 4;     /* chunkCount */
    put_be64(p, c->next_send_count++); p += 8;
    put_be16(p, AUDIO_FRAMES_PER_PKT);  p += 2;
    put_be64(p, (uint64_t)beats_first); p += 8;
    put_be64(p, (uint64_t)bp);          p += 8;
    *p++ = 1; /* kPCM_i16 */
    put_be32(p, LINK_TDM_FS_HZ); p += 4;
    *p++ = 1; /* mono */
    uint16_t numBytes = AUDIO_FRAMES_PER_PKT * 2;
    put_be16(p, numBytes); p += 2;
    for (int i = 0; i < AUDIO_FRAMES_PER_PKT; i++) {
        put_be16(p, samples[i]); p += 2;
    }
    size_t total = p - buf;

    for (int s = 0; s < c->num_subscribers; s++) {
        net_udp_send_v4_unicast(c->sub_addr[s], c->sub_port[s],
                             LINK_DISCOVERY_PORT, buf, total);
    }
}

/* ---------------------------------------------------------------- */
/* AudioBuffer decode (multi-channel)                               */
/* ---------------------------------------------------------------- */

static link_audio_sub_t *find_sub_for_channel(const link_id_t *cid) {
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        if (link_audio_subs[i].in_use &&
            memcmp(link_audio_subs[i].channel_id.bytes, cid->bytes, 8) == 0)
            return &link_audio_subs[i];
    }
    return NULL;
}

static void handle_audio_buffer(const uint8_t *p, size_t n) {
    if (n < 8 + 8 + 4 + 26 + 1 + 4 + 1 + 2) return;
    link_id_t channel_id, session_id;
    memcpy(channel_id.bytes, p, 8); p += 8; n -= 8;
    memcpy(session_id.bytes, p, 8); p += 8; n -= 8;
    uint32_t chunk_count = be32(p); p += 4; n -= 4;
    if (chunk_count == 0 || n < (size_t)chunk_count * 26) return;

    uint16_t total_frames = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        p += 8;       /* count */
        uint16_t numFrames = be16(p); p += 2;
        p += 8;       /* beginBeats */
        p += 8;       /* tempo */
        n -= 26;
        total_frames += numFrames;
    }

    if (n < 1 + 4 + 1 + 2) return;
    uint8_t  codec   = *p++; n--;
    p += 4; n -= 4;             /* sampleRate (we trust the sender) */
    uint8_t  numChan = *p++; n--;
    uint16_t numBytes= be16(p); p += 2; n -= 2;

    if (codec != 1 /*PCM_i16*/) return;
    if (numChan == 0) return;
    if ((uint32_t)total_frames * numChan * 2 != numBytes) return;
    if (n < numBytes) return;

    /* Session enforcement (§9.8): drop AudioBuffer from a different
     * Link session than ours. */
    if (memcmp(session_id.bytes, link_self_session.bytes, 8) != 0) return;

    link_audio_sub_t *sub = find_sub_for_channel(&channel_id);
    if (!sub) return;

    /* Multi-channel: we currently route channel 0 of the incoming
     * stream into the subscriber's TDM slot. Channels 1..N-1 are
     * dropped (one TDM-slot subscription is the model). A future
     * extension can subscribe to a (channel_id, plane) pair. */
    sub->num_chan_remote = numChan;
    for (int f = 0; f < total_frames; f++) {
        uint16_t v = be16(p + (f * numChan) * 2);
        sub->jbuf[sub->jbuf_w % LINK_AUDIO_JBUF_LEN] = v;
        sub->jbuf_w++;
        if (sub->jbuf_w - sub->jbuf_r > LINK_AUDIO_JBUF_LEN) {
            sub->jbuf_r = sub->jbuf_w - LINK_AUDIO_JBUF_LEN;
        }
    }
    sub->last_count++;
}

/* ---------------------------------------------------------------- */
/* RX dispatch                                                      */
/* ---------------------------------------------------------------- */

void link_audio_handle_rx(const uint8_t *buf, size_t len,
                          uint32_t src_addr, uint16_t src_port) {
    if (len < 8 + 12) return;
    if (memcmp(buf, kProtoAudio, 8) != 0) return;
    const uint8_t *p = buf + 8;
    uint8_t  mtype = *p++;
    /*uint8_t  ttl   =*/ p++;
    /*uint16_t group =*/ p += 2;
    link_id_t sender_id;
    memcpy(sender_id.bytes, p, 8); p += 8;
    if (memcmp(sender_id.bytes, link_self_id.bytes, 8) == 0) return;

    const uint8_t *body = p;
    size_t bodylen = (buf + len) - body;

    switch (mtype) {
    case 1: { /* kPeerAnnouncement */
        ann_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.peer_id   = sender_id;
        ctx.peer_addr = src_addr;
        ctx.peer_port = src_port;
        tlv_walk(body, bodylen, ann_handler, &ctx);
        if (ctx.have_ping) send_pong(src_addr, src_port, ctx.ping_ht);
        break;
    }
    case 2: /* kChannelByes */
        tlv_walk(body, bodylen, handle_aucb_entry, NULL);
        break;
    case 3: /* kPong */
        /* The body carries an echoed __ht. We could compute RTT here
         * and feed a quality filter; for now we just ignore it. */
        break;
    case 4: /* kChannelRequest */
    case 5: /* kStopChannelRequest */
        handle_channel_request(mtype, &sender_id, src_addr, src_port,
                               body, bodylen);
        break;
    case 6: /* kAudioBuffer (raw, NOT TLV) */
        handle_audio_buffer(body, bodylen);
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- */
/* TDM frame handler — accumulate samples → AudioBuffer; drain      */
/* jitter buffers into TDM TX                                       */
/* ---------------------------------------------------------------- */

void link_audio_on_tdm_frame(int port) {
    if (port >= LINK_NUM_TDM_PORTS) return;

    /* RX side: capture samples from TDM, ship one packet per channel
     * every AUDIO_FRAMES_PER_PKT frames. */
    for (int ch = 0; ch < LINK_TDM_CHANNELS_PER_PORT; ch++) {
        uint16_t s = tdm_read_rx(port, ch);
        uint32_t pos = s_tx_pkt_pos[port][ch];
        s_tx_pkt_buf[port][ch][pos++] = s;
        if (pos == AUDIO_FRAMES_PER_PKT) {
            for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
                link_audio_local_t *lc = &link_audio_local[i];
                if (lc->in_use && lc->tdm_port == port && lc->tdm_channel == ch) {
                    send_audio_buffer_for(lc, s_tx_pkt_buf[port][ch]);
                    break;
                }
            }
            pos = 0;
        }
        s_tx_pkt_pos[port][ch] = pos;
    }

    /* TX side: drain each subscription's jitter buffer into the right
     * TDM slot (one sample per frame). */
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        link_audio_sub_t *sub = &link_audio_subs[i];
        if (!sub->in_use || sub->tdm_port != port) continue;
        if (sub->jbuf_r != sub->jbuf_w) {
            uint16_t v = sub->jbuf[sub->jbuf_r % LINK_AUDIO_JBUF_LEN];
            sub->jbuf_r++;
            tdm_write_tx(sub->tdm_port, sub->tdm_channel, v);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Periodic ticks                                                   */
/* ---------------------------------------------------------------- */

void link_audio_tick(void) {
    uint64_t now = host_time_us();
    if (now - s_last_announce_us > LINK_DISCOVERY_BCAST_MS * 1000ULL) {
        send_announcements();
    }

    /* Re-issue ChannelRequest for every active subscription on the
     * §9.7 schedule. */
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        link_audio_sub_t *sub = &link_audio_subs[i];
        if (!sub->in_use) continue;
        if (now >= sub->next_request_us) {
            send_channel_request(/*kChannelRequest=*/4,
                                 sub->source_addr, sub->source_port,
                                 &sub->channel_id);
            sub->next_request_us = now + AUDIO_REQUEST_TTL_MS * 1000ULL;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Subscription API                                                 */
/* ---------------------------------------------------------------- */

int link_audio_subscribe(int my_tdm_port, int my_tdm_channel,
                         const link_id_t *channel_id) {
    /* Look up the remote channel in the catalog so we know where to
     * send the request. */
    link_audio_remote_t *r = NULL;
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        if (link_audio_remote[i].in_use &&
            memcmp(link_audio_remote[i].channel_id.bytes,
                   channel_id->bytes, 8) == 0) {
            r = &link_audio_remote[i];
            break;
        }
    }
    if (!r) return -1;

    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        if (link_audio_subs[i].in_use) continue;
        link_audio_sub_t *sub = &link_audio_subs[i];
        memset(sub, 0, sizeof(*sub));
        sub->in_use      = 1;
        sub->channel_id  = r->channel_id;
        sub->source_peer = r->peer_id;
        sub->source_addr = r->endpoint_addr;
        sub->source_port = r->endpoint_port;
        sub->tdm_port    = my_tdm_port;
        sub->tdm_channel = my_tdm_channel;
        sub->next_request_us = 0;       /* fire immediately on next tick */
        return 0;
    }
    return -2;     /* table full */
}

int link_audio_unsubscribe(const link_id_t *channel_id) {
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE; i++) {
        link_audio_sub_t *sub = &link_audio_subs[i];
        if (!sub->in_use) continue;
        if (memcmp(sub->channel_id.bytes, channel_id->bytes, 8) != 0) continue;
        send_channel_request(/*kStopChannelRequest=*/5,
                             sub->source_addr, sub->source_port,
                             &sub->channel_id);
        sub->in_use = 0;
        return 0;
    }
    return -1;
}
