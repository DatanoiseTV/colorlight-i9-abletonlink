/*
 * Wrapper around the BeatPulseGen CSRs (litex_soc/beat_pulse.py).
 *
 * The hardware computes beat-clock and transport pulses from a Timeline
 * (tempo, beat origin, time origin) plus a `run` flag and `sync_led` /
 * `peer_led` bits. The firmware programs these whenever the active
 * timeline or session state changes.
 */

#ifndef LINKFPGA_BEAT_PULSE_H
#define LINKFPGA_BEAT_PULSE_H

#include <stdint.h>
#include "link.h"

void beat_pulse_init(void);
void beat_pulse_load_timeline(const link_timeline_t *tl,
                              const link_startstop_t *ss);
void beat_pulse_set_sync_led(int on);
void beat_pulse_set_peer_led(int on);
void beat_pulse_set_beats_per_bar(int bpb);

#endif /* LINKFPGA_BEAT_PULSE_H */
