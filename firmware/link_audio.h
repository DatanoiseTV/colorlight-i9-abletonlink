/*
 * Link-Audio extension state (LINK_PROTOCOL_SPEC.md §9).
 *
 * Each TDM16 port maps to one Link-Audio "stream pair":
 *   - Channels coming OUT of the port (= TDM RX from the codec) are
 *     announced as Link-Audio channels that other peers can subscribe to.
 *   - Channels coming IN to the port (= TDM TX to the codec) can be
 *     subscribed-to from any other peer's announced channels.
 *
 * Per channel state:
 *   - 16-byte channel id (random at boot)
 *   - human name "tdmN-chK"
 *   - subscriber list (set of peers that requested this channel)
 *   - jitter buffer for inbound audio (small ring of decoded samples)
 */

#ifndef LINKFPGA_LINK_AUDIO_H
#define LINKFPGA_LINK_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include "link.h"

#define LINK_AUDIO_NAME_MAX  32
#define LINK_AUDIO_JBUF_LEN  256        /* samples per jitter ring */

typedef struct {
    int       in_use;
    link_id_t channel_id;
    char      name[LINK_AUDIO_NAME_MAX];
    int       tdm_port;
    int       tdm_channel;
    uint64_t  next_send_count;
    /* subscribers: addresses of peers that requested this channel */
    int       num_subscribers;
    uint32_t  sub_addr[8];
    uint16_t  sub_port[8];
} link_audio_channel_t;

typedef struct {
    int       in_use;
    link_id_t source_peer;
    link_id_t channel_id;
    int       tdm_port;
    int       tdm_channel;
    uint64_t  last_count;
    uint16_t  jbuf[LINK_AUDIO_JBUF_LEN];
    uint32_t  jbuf_w, jbuf_r;
} link_audio_subscription_t;

void link_audio_init(void);
void link_audio_tick(void);
void link_audio_handle_rx(const uint8_t *buf, size_t len,
                          uint32_t src_addr, uint16_t src_port);
void link_audio_on_tdm_frame(int port);
void link_audio_request_channel(int my_tdm_port, int my_tdm_channel,
                                const link_id_t *peer,
                                const link_id_t *channel_id);

#endif /* LINKFPGA_LINK_AUDIO_H */
