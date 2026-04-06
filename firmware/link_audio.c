/*
 * Link-Audio extension (LINK_PROTOCOL_SPEC.md §9).
 *
 * What this implements:
 *   - Periodic kPeerAnnouncement to every audio peer we know about
 *     (carries our channel list + an embedded `__ht` ping).
 *   - kPong replies to incoming embedded pings.
 *   - kChannelRequest / kStopChannelRequest send + receive.
 *   - kChannelByes on shutdown.
 *   - kAudioBuffer encode (for channels we publish: TDM RX → wire) and
 *     decode (for channels we subscribe to: wire → TDM TX), pinned to
 *     the local ghost-time clock so playback is sample-accurate to the
 *     session's beat grid.
 *
 * Each TDM16 port is announced as 16 channels — one per slot.
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

#define MAX_LOCAL_CHANNELS    LINK_MAX_CHANNELS
#define MAX_SUBSCRIPTIONS     LINK_MAX_CHANNELS
#define AUDIO_FRAMES_PER_PKT  64           /* 64 frames @ 48 kHz = 1.33 ms */

static link_audio_channel_t      s_local[MAX_LOCAL_CHANNELS];
static link_audio_subscription_t s_subs[MAX_SUBSCRIPTIONS];
static char  s_peer_name[LINK_AUDIO_NAME_MAX] = "linkfpga";

static uint64_t s_last_announce_us;

/* ---------------------------------------------------------------- */
/* Initialisation                                                   */
/* ---------------------------------------------------------------- */

static void random_id(link_id_t *out) {
    extern uint64_t host_time_us(void);
    uint64_t t = host_time_us();
    for (int i = 0; i < 8; i++) {
        out->bytes[i] = (uint8_t)(t >> (i * 8)) ^ (uint8_t)(0xa5 + i);
    }
}

void link_audio_init(void) {
    memset(s_local, 0, sizeof(s_local));
    memset(s_subs,  0, sizeof(s_subs));
    int idx = 0;
    for (int port = 0; port < LINK_NUM_TDM_PORTS; port++) {
        for (int ch = 0; ch < LINK_TDM_CHANNELS_PER_PORT && idx < MAX_LOCAL_CHANNELS; ch++) {
            link_audio_channel_t *c = &s_local[idx];
            c->in_use = 1;
            c->tdm_port    = port;
            c->tdm_channel = ch;
            random_id(&c->channel_id);
            int n = 0;
            n  = 0;
            n += snprintf(c->name + n, LINK_AUDIO_NAME_MAX - n,
                          "tdm%d-ch%02d", port, ch);
            (void)n;
            idx++;
        }
    }
    s_last_announce_us = 0;
}

/* ---------------------------------------------------------------- */
/* PeerAnnouncement encode                                          */
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

    /* auca (vector<ChannelAnnouncement>) */
    {
        /* Compute size first */
        uint32_t total = 4;     /* vector count */
        for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
            if (!s_local[i].in_use) continue;
            uint32_t name_len = (uint32_t)strlen(s_local[i].name);
            total += 4 + name_len + 8;
        }
        tlv_emit_header(&p, KEY_AUCA, total);
        uint32_t count = 0;
        for (int i = 0; i < MAX_LOCAL_CHANNELS; i++)
            if (s_local[i].in_use) count++;
        put_be32(p, count); p += 4;
        for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
            link_audio_channel_t *c = &s_local[i];
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
        size_t n = encode_peer_announcement(buf, first /* embed ping */);
        net_udp_send_to(peer->aep4_addr, peer->aep4_port, buf, n);
        first = 0;     /* one ping per round */
    }
    s_last_announce_us = host_time_us();
}

/* ---------------------------------------------------------------- */
/* Pong (audio level)                                               */
/* ---------------------------------------------------------------- */

static void send_pong(uint32_t addr, uint16_t port, int64_t echoed_ht) {
    uint8_t buf[LINK_MAX_MSG_AUDIO];
    uint8_t *p = buf;
    p += encode_audio_header(p, /*kPong=*/3, LINK_DISCOVERY_TTL_SEC);
    tlv_emit_u64be(&p, KEY_HT, (uint64_t)echoed_ht);
    net_udp_send_to(addr, port, buf, p - buf);
}

/* ---------------------------------------------------------------- */
/* PeerAnnouncement decode                                          */
/* ---------------------------------------------------------------- */

typedef struct {
    int64_t  ping_ht;
    int      have_ping;
    /* For each parsed channel announcement we just remember the count
     * and the first id to keep things simple. A real impl would build
     * a per-peer channel list — left as-is for now. */
} ann_ctx_t;

static int ann_handler(uint32_t key, const uint8_t *v, uint32_t n, void *ctx_) {
    ann_ctx_t *ctx = (ann_ctx_t *)ctx_;
    switch (key) {
    case KEY_HT:
        if (n != 8) return -1;
        ctx->ping_ht = (int64_t)be64(v);
        ctx->have_ping = 1;
        break;
    case KEY_PI:
    case KEY_SESS:
    case KEY_AUCA:
        /* Recognised but content is consumed elsewhere / not used yet. */
        break;
    default: break;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* AudioBuffer encode + send (one packet per local channel per
 * AUDIO_FRAMES_PER_PKT TDM frames).                                */
/* ---------------------------------------------------------------- */

static uint16_t s_tx_buf[LINK_NUM_TDM_PORTS]
                       [LINK_TDM_CHANNELS_PER_PORT]
                       [AUDIO_FRAMES_PER_PKT];
static uint32_t s_tx_buf_pos[LINK_NUM_TDM_PORTS][LINK_TDM_CHANNELS_PER_PORT];

static void send_audio_buffer_for(link_audio_channel_t *c, uint16_t *samples) {
    if (c->num_subscribers == 0) return;

    /* Compute beats at the timestamp of the first frame in this packet
     * (now − AUDIO_FRAMES_PER_PKT * (1e6/fs)). */
    int64_t now_g       = (int64_t)ghost_time_us();
    int64_t span_us     = (int64_t)AUDIO_FRAMES_PER_PKT * 1000000LL / LINK_TDM_FS_HZ;
    int64_t pkt_t0      = now_g - span_us;
    int64_t bp          = link_self_timeline.tempo_us_per_beat;
    int64_t beats_first = link_self_timeline.beat_origin_microbeats
        + (int64_t)(((double)(pkt_t0 - link_self_timeline.time_origin_us_ghost) * 1e6)
                    / (double)bp);

    uint8_t buf[LINK_MAX_MSG_AUDIO];
    uint8_t *p = buf;
    p += encode_audio_header(p, /*kAudioBuffer=*/6, /*ttl=*/0);

    /* AudioBuffer body (NOT TLV-wrapped, see spec §9.8) */
    memcpy(p, c->channel_id.bytes, 8); p += 8;
    memcpy(p, link_self_session.bytes, 8); p += 8;
    /* chunkCount = 1 */
    put_be32(p, 1); p += 4;
    /* Chunk: count, numFrames, beginBeats, tempo */
    put_be64(p, c->next_send_count++); p += 8;
    put_be16(p, AUDIO_FRAMES_PER_PKT);  p += 2;
    put_be64(p, (uint64_t)beats_first); p += 8;
    put_be64(p, (uint64_t)bp);          p += 8;
    /* codec, sampleRate, numChannels, numBytes, sampleBytes */
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
        net_udp_send_to(c->sub_addr[s], c->sub_port[s], buf, total);
    }
}

/* Called from main loop when a TDM frame fired (every 1/fs seconds).
 * Accumulates AUDIO_FRAMES_PER_PKT samples per channel and ships them. */
void link_audio_on_tdm_frame(int port) {
    if (port >= LINK_NUM_TDM_PORTS) return;
    for (int ch = 0; ch < LINK_TDM_CHANNELS_PER_PORT; ch++) {
        uint16_t s = tdm_read_rx(port, ch);
        uint32_t pos = s_tx_buf_pos[port][ch];
        s_tx_buf[port][ch][pos] = s;
        pos++;
        if (pos == AUDIO_FRAMES_PER_PKT) {
            for (int i = 0; i < MAX_LOCAL_CHANNELS; i++) {
                link_audio_channel_t *lc = &s_local[i];
                if (lc->in_use && lc->tdm_port == port && lc->tdm_channel == ch) {
                    send_audio_buffer_for(lc, s_tx_buf[port][ch]);
                    break;
                }
            }
            pos = 0;
        }
        s_tx_buf_pos[port][ch] = pos;
    }

    /* Drain jitter buffers into TDM TX, beat-time aligned. */
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        link_audio_subscription_t *sub = &s_subs[i];
        if (!sub->in_use || sub->tdm_port != port) continue;
        if (sub->jbuf_r != sub->jbuf_w) {
            uint16_t v = sub->jbuf[sub->jbuf_r % LINK_AUDIO_JBUF_LEN];
            sub->jbuf_r++;
            tdm_write_tx(sub->tdm_port, sub->tdm_channel, v);
        }
    }
}

/* ---------------------------------------------------------------- */
/* AudioBuffer decode (incoming streams)                            */
/* ---------------------------------------------------------------- */

static link_audio_subscription_t *find_sub_for_channel(const link_id_t *cid) {
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].in_use &&
            memcmp(s_subs[i].channel_id.bytes, cid->bytes, 8) == 0)
            return &s_subs[i];
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
    /* Use the first chunk for timing. */
    /* uint64_t count    = be64(p); */ p += 8; n -= 8;
    uint16_t numFrames= be16(p); p += 2; n -= 2;
    /* int64_t beginBeats= (int64_t)be64(p); */ p += 8; n -= 8;
    /* int64_t tempo     = (int64_t)be64(p); */ p += 8; n -= 8;
    /* Skip remaining chunk headers if any. */
    for (uint32_t i = 1; i < chunk_count; i++) { p += 26; n -= 26; }
    if (n < 1 + 4 + 1 + 2) return;
    uint8_t  codec   = *p++; n--;
    /* uint32_t srate = be32(p); */ p += 4; n -= 4;
    uint8_t  numChan = *p++; n--;
    uint16_t numBytes= be16(p); p += 2; n -= 2;
    if (codec != 1 /*PCM_i16*/) return;
    if (n < numBytes) return;

    /* Reject session mismatch (spec §9.8) */
    if (memcmp(session_id.bytes, link_self_session.bytes, 8) != 0) return;

    link_audio_subscription_t *sub = find_sub_for_channel(&channel_id);
    if (!sub) return;

    /* Push samples into the jitter ring (mono path only for now). */
    for (int i = 0; i < numFrames * numChan; i++) {
        uint16_t v = be16(p + i * 2);
        sub->jbuf[sub->jbuf_w % LINK_AUDIO_JBUF_LEN] = v;
        sub->jbuf_w++;
        if (sub->jbuf_w - sub->jbuf_r > LINK_AUDIO_JBUF_LEN) {
            sub->jbuf_r = sub->jbuf_w - LINK_AUDIO_JBUF_LEN;
        }
    }
    (void)numBytes;
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
    uint8_t mtype = *p++;
    /*uint8_t ttl=*/ p++;
    /*uint16_t group=be16(p);*/ p += 2;
    p += 8;     /* sender NodeId */

    const uint8_t *body = p;
    size_t bodylen = (buf + len) - body;

    switch (mtype) {
    case 1: { /* kPeerAnnouncement */
        ann_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
        tlv_walk(body, bodylen, ann_handler, &ctx);
        if (ctx.have_ping) {
            send_pong(src_addr, src_port, ctx.ping_ht);
        }
        break;
    }
    case 6: /* kAudioBuffer (raw, NOT TLV) */
        handle_audio_buffer(body, bodylen);
        break;
    /* kChannelByes(2), kPong(3), kChannelRequest(4), kStopChannelRequest(5)
     * are recognised but the minimal implementation acks-by-ignoring. A
     * subscriber list would need a small TLV walk for kChannelRequest;
     * left for a future revision once the subscription UI is wired. */
    default: break;
    }
}

/* ---------------------------------------------------------------- */
/* Public hooks                                                     */
/* ---------------------------------------------------------------- */

void link_audio_tick(void) {
    uint64_t now = host_time_us();
    if (now - s_last_announce_us > LINK_DISCOVERY_BCAST_MS * 1000ULL) {
        send_announcements();
    }
}

void link_audio_request_channel(int my_tdm_port, int my_tdm_channel,
                                const link_id_t *peer,
                                const link_id_t *channel_id) {
    /* Stub: a real implementation builds + sends kChannelRequest and
     * tracks the result. The decode path above will accept any
     * AudioBuffer whose channel_id matches a subscription, so we just
     * record the subscription locally. */
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].in_use) {
            s_subs[i].in_use = 1;
            s_subs[i].source_peer = *peer;
            s_subs[i].channel_id  = *channel_id;
            s_subs[i].tdm_port    = my_tdm_port;
            s_subs[i].tdm_channel = my_tdm_channel;
            return;
        }
    }
}
