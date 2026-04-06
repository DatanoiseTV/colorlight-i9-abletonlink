#include "beat_pulse.h"
#include "config.h"

static uint8_t s_flags;     /* run, sync_led, peer_led mirror */
static int     s_beats_per_bar = 4;

void beat_pulse_init(void) {
    s_flags = 0;
    beat_pulse_flags_write(s_flags);
    beat_pulse_pulse_width_write(1000);     /* 1 ms */
}

void beat_pulse_load_timeline(const link_timeline_t *tl,
                              const link_startstop_t *ss) {
    /* Compute pulse periods in microseconds from the timeline. The
     * gateware just down-counts these — no runtime divide in HW. */
    int64_t us_per_beat = tl->tempo_us_per_beat;
    if (us_per_beat <= 0) us_per_beat = 500000;     /* fallback 120 BPM */

    beat_pulse_period_beat_write    ((uint32_t)(us_per_beat));
    beat_pulse_period_24ppqn_write  ((uint32_t)(us_per_beat / 24));
    beat_pulse_period_48ppqn_write  ((uint32_t)(us_per_beat / 48));
    beat_pulse_period_96ppqn_write  ((uint32_t)(us_per_beat / 96));
    beat_pulse_period_bar_write     ((uint32_t)(us_per_beat * s_beats_per_bar));

    /* Reset all counters so the next pulse aligns to the new tempo
     * grid. */
    beat_pulse_soft_reset_write(0x01);

    if (ss->is_playing) s_flags |=  0x01;
    else                s_flags &= ~0x01;
    beat_pulse_flags_write(s_flags);
}

void beat_pulse_set_sync_led(int on) {
    if (on) s_flags |=  0x08;
    else    s_flags &= ~0x08;
    beat_pulse_flags_write(s_flags);
}

void beat_pulse_set_peer_led(int on) {
    if (on) s_flags |=  0x10;
    else    s_flags &= ~0x10;
    beat_pulse_flags_write(s_flags);
}

void beat_pulse_set_beats_per_bar(int bpb) {
    if (bpb < 1) bpb = 1;
    s_beats_per_bar = bpb;
}
