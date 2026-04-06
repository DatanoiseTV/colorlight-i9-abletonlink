/*
 * Link-Audio extension state (LINK_PROTOCOL_SPEC.md §9).
 *
 * Each TDM16 port maps to one Link-Audio "stream pair":
 *   - Channels coming OUT of the port (= TDM RX from the codec) are
 *     announced as Link-Audio channels that other peers can subscribe to.
 *   - Channels coming IN to the port (= TDM TX to the codec) can be
 *     subscribed-to from any other peer's announced channels.
 *
 * State held per peer / channel:
 *
 *   link_audio_local_t      — channels we publish (TDM RX → wire)
 *                              + per-channel subscriber set
 *   link_audio_remote_t     — channels OTHER peers have announced to
 *                              us, with the (peer, channel) ids and
 *                              the human-readable name; this is the
 *                              menu the user picks from when wiring an
 *                              external channel into a TDM TX slot
 *   link_audio_sub_t        — channels we have subscribed to, each
 *                              with a small jitter ring and a
 *                              (port, channel) routing target
 */

#ifndef LINKFPGA_LINK_AUDIO_H
#define LINKFPGA_LINK_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include "link.h"

#define LINK_AUDIO_NAME_MAX     32
#define LINK_AUDIO_JBUF_LEN     1024     /* per-subscription jitter ring */
#define LINK_AUDIO_MAX_SUB_PER  8        /* subscribers per local channel */
#define LINK_AUDIO_MAX_REMOTE   64       /* known remote channels (catalog) */

typedef struct {
    int       in_use;
    link_id_t channel_id;
    char      name[LINK_AUDIO_NAME_MAX];
    int       tdm_port;
    int       tdm_channel;
    uint64_t  next_send_count;
    int       num_subscribers;
    uint32_t  sub_addr[LINK_AUDIO_MAX_SUB_PER];
    uint16_t  sub_port[LINK_AUDIO_MAX_SUB_PER];
    link_id_t sub_peer[LINK_AUDIO_MAX_SUB_PER];
} link_audio_local_t;

typedef struct {
    int       in_use;
    link_id_t peer_id;          /* announcing peer */
    link_id_t channel_id;
    char      name[LINK_AUDIO_NAME_MAX];
    uint32_t  endpoint_addr;    /* aep4 of the announcing peer */
    uint16_t  endpoint_port;
    uint64_t  last_seen_us;
} link_audio_remote_t;

typedef struct {
    int       in_use;
    link_id_t channel_id;
    link_id_t source_peer;
    uint32_t  source_addr;
    uint16_t  source_port;
    int       tdm_port;
    int       tdm_channel;
    uint64_t  next_request_us;  /* re-request schedule */
    uint64_t  last_count;
    uint8_t   num_chan_remote;
    uint16_t  jbuf[LINK_AUDIO_JBUF_LEN];
    uint32_t  jbuf_w, jbuf_r;
} link_audio_sub_t;

extern link_audio_local_t  link_audio_local [];
extern link_audio_remote_t link_audio_remote[];
extern link_audio_sub_t    link_audio_subs  [];

void link_audio_init(void);
void link_audio_tick(void);
void link_audio_handle_rx(const uint8_t *buf, size_t len,
                          uint32_t src_addr, uint16_t src_port);
void link_audio_on_tdm_frame(int port);

/* Subscribe a remote channel into a local TDM TX slot. Returns 0 on
 * success. The subscription is reactive: link_audio_tick() will
 * (re)issue ChannelRequest messages every LINK_AUDIO_REQUEST_TTL
 * seconds while the subscription is alive. */
int  link_audio_subscribe(int my_tdm_port, int my_tdm_channel,
                          const link_id_t *channel_id);
int  link_audio_unsubscribe(const link_id_t *channel_id);

#endif /* LINKFPGA_LINK_AUDIO_H */
