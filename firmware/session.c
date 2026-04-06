/*
 * Session supervisor — drives Link's session election and the periodic
 * remeasurement schedule (LINK_PROTOCOL_SPEC.md §6).
 *
 * Strategy:
 *   - When we see a peer claiming a session different from ours and we
 *     do not yet have a measurement for it, kick a measurement against
 *     that peer (preferring the founder).
 *   - On a successful measurement, decide whether to switch sessions
 *     using the `SESSION_EPS` rule.
 *   - Re-measure the active session every 30 s.
 */

#include <string.h>
#include <stdlib.h>

#include "link.h"
#include "ghost_time.h"
#include "beat_pulse.h"
#include "config.h"

extern void measurement_start(uint32_t addr, uint16_t port,
                              const link_id_t *session_id,
                              void (*completion)(int success, int64_t intercept));
extern void measurement_tick(void);

static uint64_t s_last_remeasure_us;

static void on_measure_done(int success, int64_t intercept);

static link_id_t s_active_target_session;     /* what we just tried to measure */

static link_peer_t *find_founder(const link_id_t *session_id) {
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use &&
            memcmp(p->session_id.bytes, session_id->bytes, 8) == 0 &&
            memcmp(p->node_id.bytes,    session_id->bytes, 8) == 0)
            return p;
    }
    /* fallback: any peer in that session */
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use &&
            memcmp(p->session_id.bytes, session_id->bytes, 8) == 0)
            return p;
    }
    return NULL;
}

static int session_known_in_local_view(const link_id_t *session_id) {
    if (memcmp(session_id->bytes, link_self_session.bytes, 8) == 0) return 1;
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use && p->measured &&
            memcmp(p->session_id.bytes, session_id->bytes, 8) == 0)
            return 1;
    }
    return 0;
}

void session_observe_peer(link_peer_t *peer) {
    if (!peer) return;
    /* Same session as ours — nothing to do. */
    if (memcmp(peer->session_id.bytes, link_self_session.bytes, 8) == 0)
        return;
    /* Already measured this session — nothing to do until next remeasure. */
    if (session_known_in_local_view(&peer->session_id)) return;

    /* Pick the founder (or, failing that, any peer in that session) and
     * launch a measurement. */
    link_peer_t *target = find_founder(&peer->session_id);
    if (!target || target->mep4_addr == 0) return;
    s_active_target_session = peer->session_id;
    measurement_start(target->mep4_addr, target->mep4_port,
                      &peer->session_id, on_measure_done);
}

static void on_measure_done(int success, int64_t intercept) {
    if (!success) return;

    int64_t cur_intercept = ghost_time_get_intercept();
    int64_t now_host  = (int64_t)host_time_us();
    int64_t cur_ghost = now_host + cur_intercept;
    int64_t new_ghost = now_host + intercept;
    int64_t diff      = new_ghost - cur_ghost;

    int switch_to_new = 0;
    if (diff > LINK_SESSION_EPS_US) {
        switch_to_new = 1;
    } else if (llabs(diff) < LINK_SESSION_EPS_US) {
        if (memcmp(s_active_target_session.bytes,
                   link_self_session.bytes, 8) < 0)
            switch_to_new = 1;
    }

    /* Mark this session as measured in the local view, regardless of
     * whether we switch to it. */
    for (int i = 0; i < LINK_MAX_PEERS; i++) {
        link_peer_t *p = &link_peers[i];
        if (p->in_use && memcmp(p->session_id.bytes,
                                s_active_target_session.bytes, 8) == 0) {
            p->measured     = 1;
            p->ghost_offset = intercept;
        }
    }

    if (switch_to_new) {
        link_self_session = s_active_target_session;
        ghost_time_set_intercept(intercept);

        /* Adopt the active timeline of any peer in the new session.
         * Latest time_origin wins (LINK_PROTOCOL_SPEC.md §6.6). */
        link_peer_t *best = NULL;
        for (int i = 0; i < LINK_MAX_PEERS; i++) {
            link_peer_t *p = &link_peers[i];
            if (p->in_use && memcmp(p->session_id.bytes,
                                    link_self_session.bytes, 8) == 0) {
                if (!best ||
                    p->timeline.time_origin_us_ghost
                      > best->timeline.time_origin_us_ghost)
                    best = p;
            }
        }
        if (best) {
            link_self_timeline = best->timeline;
            link_self_startstop = best->startstop;
            beat_pulse_load_timeline(&link_self_timeline,
                                     &link_self_startstop);
        }
        beat_pulse_set_sync_led(1);
        link_broadcast_alive_now();
    }

    s_last_remeasure_us = host_time_us();
}

void session_tick(void) {
    measurement_tick();

    /* Periodic remeasurement of the active session, if we are joined to
     * a foreign session (the founder is some peer ≠ self). */
    if (memcmp(link_self_session.bytes, link_self_id.bytes, 8) == 0) {
        /* We are the founder of our own session — no remeasure. */
        return;
    }
    uint64_t now = host_time_us();
    if (now - s_last_remeasure_us >
        (uint64_t)LINK_SESSION_REMEASURE_S * 1000000ULL) {
        link_peer_t *target = find_founder(&link_self_session);
        if (target) {
            s_active_target_session = link_self_session;
            measurement_start(target->mep4_addr, target->mep4_port,
                              &link_self_session, on_measure_done);
        }
        s_last_remeasure_us = now;
    }

    /* Update peer LED based on whether anyone else is around. */
    int peers = 0;
    for (int i = 0; i < LINK_MAX_PEERS; i++)
        if (link_peers[i].in_use) peers++;
    beat_pulse_set_peer_led(peers > 0);
}
