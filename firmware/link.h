/*
 * Link discovery protocol (LINK_PROTOCOL_SPEC.md §5) state.
 *
 * link_init()       must be called once at boot, after the network is up.
 * link_tick()       must be called once per main-loop iteration; it walks
 *                   timers and emits ALIVE / RESPONSE / BYEBYE as needed.
 * link_handle_rx()  must be called for every UDP datagram that the
 *                   LinkPacketFilter classified as a Discovery v1 packet.
 *
 * The peer table is a flat array (LINK_MAX_PEERS) since we expect at
 * most a handful of Link nodes on a studio LAN.
 */

#ifndef LINKFPGA_LINK_H
#define LINKFPGA_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

typedef struct {
    uint8_t bytes[8];
} link_id_t;

typedef struct {
    int64_t  tempo_us_per_beat;
    int64_t  beat_origin_microbeats;
    int64_t  time_origin_us_ghost;
} link_timeline_t;

typedef struct {
    uint8_t  is_playing;
    int64_t  beats_microbeats;
    int64_t  timestamp_us_ghost;
} link_startstop_t;

typedef struct {
    int      in_use;
    link_id_t       node_id;
    link_id_t       session_id;
    link_timeline_t timeline;
    link_startstop_t startstop;
    uint32_t mep4_addr;       /* host byte order */
    uint16_t mep4_port;
    uint32_t aep4_addr;
    uint16_t aep4_port;
    uint8_t  mep6_addr[16];   /* IPv6 measurement endpoint */
    uint16_t mep6_port;
    int      have_mep6;
    uint8_t  aep6_addr[16];   /* IPv6 audio endpoint */
    uint16_t aep6_port;
    int      have_aep6;
    uint64_t expires_at_us;   /* host time */
    int      measured;        /* 1 once we have a GhostXForm for this session */
    int64_t  ghost_offset;    /* GhostXForm intercept for that session */
} link_peer_t;

extern link_id_t        link_self_id;
extern link_id_t        link_self_session;
extern link_timeline_t  link_self_timeline;
extern link_startstop_t link_self_startstop;
extern link_peer_t      link_peers[LINK_MAX_PEERS];

void link_init(void);
void link_tick(void);
void link_handle_rx(const uint8_t *buf, size_t len,
                    uint32_t src_addr, uint16_t src_port);
void link_broadcast_alive_now(void);
void link_send_byebye(void);

/* Apply a Timeline edit locally + push it into BeatPulseGen + broadcast. */
void link_set_local_tempo(double bpm);
void link_set_play(int play);

#endif /* LINKFPGA_LINK_H */
