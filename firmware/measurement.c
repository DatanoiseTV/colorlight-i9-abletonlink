/*
 * Link v1 ping/pong measurement (LINK_PROTOCOL_SPEC.md §7).
 *
 * Single-measurement-at-a-time state machine. The session supervisor
 * (`session.c`) calls `measurement_start()` against a chosen target
 * peer; this module then drives PING / PONG until either it has
 * collected enough samples (success) or the round budget is exhausted
 * (failure). On success it computes the median of the offset samples
 * and applies it as the new GhostXForm intercept for the local node,
 * then notifies session.c.
 */

#include <string.h>
#include <stdlib.h>

#include "link.h"
#include "tlv.h"
#include "net.h"
#include "ghost_time.h"
#include "config.h"

extern int64_t median_i64(int64_t *a, size_t n);

static const uint8_t kProtoLink[8] = { '_','l','i','n','k','_','v',0x01 };

#define MAX_SAMPLES 200

typedef struct {
    int      active;
    uint32_t target_addr;
    uint16_t target_port;
    link_id_t target_session;
    int      pings_sent;
    uint64_t next_send_us;
    int64_t  prev_ghost_time;
    int64_t  prev_host_time;
    int64_t  samples[MAX_SAMPLES];
    int      n_samples;
    void   (*completion)(int success, int64_t intercept);
} state_t;

static state_t S;

static size_t encode_ping(uint8_t *buf, int64_t host_time,
                          int64_t prev_ghost_time, int has_prev) {
    uint8_t *p = buf;
    memcpy(p, kProtoLink, 8); p += 8;
    *p++ = 1; /* PING */
    tlv_emit_u64be(&p, KEY_HT, (uint64_t)host_time);
    if (has_prev) tlv_emit_u64be(&p, KEY_PGT, (uint64_t)prev_ghost_time);
    return p - buf;
}

static void send_ping(void) {
    uint8_t buf[LINK_MAX_MSG_LINK];
    int64_t now = (int64_t)host_time_us();
    size_t n = encode_ping(buf, now, S.prev_ghost_time, S.pings_sent > 0);
    net_udp_send_to(S.target_addr, S.target_port, buf, n);
    S.prev_host_time = now;
    S.pings_sent++;
    S.next_send_us = host_time_us() + LINK_PING_INTERVAL_MS * 1000ULL;
}

void measurement_start(uint32_t addr, uint16_t port,
                       const link_id_t *session_id,
                       void (*completion)(int success, int64_t intercept)) {
    if (S.active) return;
    memset(&S, 0, sizeof(S));
    S.active        = 1;
    S.target_addr   = addr;
    S.target_port   = port;
    S.target_session= *session_id;
    S.completion    = completion;
    send_ping();
}

void measurement_tick(void) {
    if (!S.active) return;
    uint64_t now = host_time_us();
    if (now >= S.next_send_us) {
        if (S.pings_sent >= LINK_MAX_PINGS) {
            S.active = 0;
            if (S.completion) S.completion(0, 0);
            return;
        }
        send_ping();
    }
}

typedef struct {
    int64_t  ht, gt, pgt;
    int      have_ht, have_gt, have_pgt;
    link_id_t sess;
    int      have_sess;
} pong_ctx_t;

static int pong_handler(uint32_t key, const uint8_t *v, uint32_t n, void *ctx_) {
    pong_ctx_t *ctx = (pong_ctx_t *)ctx_;
    switch (key) {
    case KEY_HT:
        if (n != 8) return -1;
        ctx->ht = (int64_t)be64(v); ctx->have_ht = 1; break;
    case KEY_GT:
        if (n != 8) return -1;
        ctx->gt = (int64_t)be64(v); ctx->have_gt = 1; break;
    case KEY_PGT:
        if (n != 8) return -1;
        ctx->pgt = (int64_t)be64(v); ctx->have_pgt = 1; break;
    case KEY_SESS:
        if (n != 8) return -1;
        memcpy(ctx->sess.bytes, v, 8); ctx->have_sess = 1; break;
    default: break;
    }
    return 0;
}

void measurement_handle_rx(const uint8_t *buf, size_t len,
                           uint32_t src_addr, uint16_t src_port) {
    if (!S.active) return;
    if (src_addr != S.target_addr || src_port != S.target_port) return;
    if (len < 8 + 1) return;
    if (memcmp(buf, kProtoLink, 8) != 0) return;
    if (buf[8] != 2 /*PONG*/) return;

    pong_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    if (tlv_walk(buf + 9, len - 9, pong_handler, &ctx) != 0) return;
    if (!ctx.have_ht || !ctx.have_gt || !ctx.have_sess) return;

    /* Session must still match. */
    if (memcmp(ctx.sess.bytes, S.target_session.bytes, 8) != 0) {
        S.active = 0;
        if (S.completion) S.completion(0, 0);
        return;
    }

    int64_t now_host = (int64_t)host_time_us();

    /* Sample 1: gt - (now + prev) / 2 */
    if (S.n_samples < MAX_SAMPLES) {
        S.samples[S.n_samples++] = ctx.gt - (now_host + ctx.ht) / 2;
    }
    /* Sample 2 (if we have a previous gt to average): (gt + pgt)/2 - prev_host */
    if (ctx.have_pgt && S.prev_host_time != 0 && S.n_samples < MAX_SAMPLES) {
        S.samples[S.n_samples++] = (ctx.gt + ctx.pgt) / 2 - ctx.ht;
    }
    S.prev_ghost_time = ctx.gt;

    if (S.n_samples > LINK_SAMPLE_THRESHOLD) {
        int64_t intercept = median_i64(S.samples, S.n_samples);
        S.active = 0;
        if (S.completion) S.completion(1, intercept);
        return;
    }

    /* Send another ping immediately. */
    send_ping();
}
