/*
 * MIDI sync state machine + thin wrapper over the MidiCore CSRs.
 *
 * Most of the work is in hardware (litex_soc/midi.py). The firmware:
 *
 *   - configures the auto-TX bits (which BeatPulseGen pulses get
 *     forwarded as MIDI System Real-Time bytes)
 *   - drains the hardware "realtime event" slot once per main-loop
 *     pass and, in MIDI_SYNC_FOLLOWER mode, sets the local Link
 *     tempo to match the incoming clock
 *   - handles incoming START / CONTINUE / STOP by toggling the local
 *     play state via `link_set_play()`
 *   - exposes the latest measured incoming BPM for the web UI
 */

#include <stdint.h>
#include <stddef.h>

#include "midi.h"
#include "link.h"
#include "ghost_time.h"
#include "config.h"

/* MidiCore CSR accessors come from generated/csr.h. */

static midi_sync_mode_t s_sync_mode = MIDI_SYNC_OBSERVE;
static double           s_observed_bpm;
static uint32_t         s_observed_period_us;
static uint64_t         s_last_clock_ts;

void midi_init(void) {
    midi_ctrl_write(0x01);                      /* enable */
    midi_auto_tx_ctrl_write(MIDI_AUTO_CLOCK
                          | MIDI_AUTO_START
                          | MIDI_AUTO_STOP);
    s_sync_mode          = MIDI_SYNC_OBSERVE;
    s_observed_bpm       = 0.0;
    s_observed_period_us = 0;
    s_last_clock_ts      = 0;
}

int midi_send_byte(uint8_t b) {
    if (midi_tx_free_read() == 0) return -1;
    midi_tx_data_write(b);
    midi_tx_push_write(1);                      /* the write itself is the push */
    return 0;
}

void midi_set_auto_tx(uint8_t flags) {
    midi_auto_tx_ctrl_write(flags & 0x07);
}

uint8_t midi_get_auto_tx(void) {
    return midi_auto_tx_ctrl_read() & 0x07;
}

int midi_recv_byte(uint8_t *out) {
    if (midi_rx_avail_read() == 0) return -1;
    *out = midi_rx_data_read();
    midi_rx_pop_write(1);
    return 0;
}

void midi_set_sync_mode(midi_sync_mode_t m) { s_sync_mode = m; }
midi_sync_mode_t midi_get_sync_mode(void)   { return s_sync_mode; }
double   midi_observed_bpm(void)            { return s_observed_bpm; }
uint32_t midi_observed_period_us(void)      { return s_observed_period_us; }
uint64_t midi_last_clock_ts_us(void)        { return s_last_clock_ts; }

/* Drain the hardware RT event slot. There's only one slot but the HW
 * timestamp is exact (latched at the stop-bit edge), so we don't need
 * a deeper queue — the firmware just needs to drain at >24 PPQN rate
 * (~50 Hz), which the main loop trivially exceeds. */
void midi_tick(void) {
    /* Live BPM from the hardware period meter. */
    uint32_t period = midi_clk_period_read();
    if (period > 0 && period != s_observed_period_us) {
        s_observed_period_us = period;
        /* 24 PPQN → bpm = 60_000_000 / (period * 24) */
        s_observed_bpm = 60000000.0 / ((double)period * 24.0);
    }

    if (!midi_rt_ev_valid_read()) return;

    /* Latched timestamp for atomicity: read TS_LO first; the SoC
     * snapshots the rest at that moment. */
    uint8_t  byte = midi_rt_ev_byte_read();
    uint32_t lo   = midi_rt_ev_ts_lo_read();
    uint32_t hi   = midi_rt_ev_ts_hi_read();
    uint64_t ts   = ((uint64_t)hi << 32) | lo;
    midi_rt_ev_ack_write(1);                    /* ack one event */

    switch (byte) {
    case MIDI_RT_CLOCK:
        s_last_clock_ts = ts;
        if (s_sync_mode == MIDI_SYNC_FOLLOWER && s_observed_bpm > 0) {
            link_set_local_tempo(s_observed_bpm);
        }
        break;
    case MIDI_RT_START:
    case MIDI_RT_CONTINUE:
        if (s_sync_mode != MIDI_SYNC_OFF)
            link_set_play(1);
        break;
    case MIDI_RT_STOP:
        if (s_sync_mode != MIDI_SYNC_OFF)
            link_set_play(0);
        break;
    default:
        /* Active sensing / system reset / other RT — ignore. */
        break;
    }
}
