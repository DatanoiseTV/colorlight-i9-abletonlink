/*
 * Thin wrapper around the MidiCore CSRs (litex_soc/midi.py).
 *
 * The hardware does the heavy lifting:
 *   - Auto-injects 0xF8 / 0xFA / 0xFC bytes off BeatPulseGen pulses.
 *     We just enable / disable that via `midi_set_auto_tx()`.
 *   - Timestamps incoming RT bytes with the GhostTime µs counter.
 *   - Computes the period of incoming 0xF8 clocks in hardware.
 *
 * The firmware just:
 *   - Calls `midi_init()` once at boot.
 *   - Calls `midi_tick()` from the main loop to drain any RT events
 *     and feed them to the sync state machine.
 *   - Calls `midi_send_byte()` to push regular MIDI bytes (note on/off,
 *     CC, sysex). These coexist with the auto-injected RT bytes — the
 *     hardware arbiter ensures real-time bytes always have priority.
 */

#ifndef LINKFPGA_MIDI_H
#define LINKFPGA_MIDI_H

#include <stddef.h>
#include <stdint.h>

/* MIDI System Real-Time message bytes. */
#define MIDI_RT_CLOCK    0xF8
#define MIDI_RT_START    0xFA
#define MIDI_RT_CONTINUE 0xFB
#define MIDI_RT_STOP     0xFC
#define MIDI_RT_ACTIVE   0xFE
#define MIDI_RT_RESET    0xFF

/* Auto-TX flags (set via midi_set_auto_tx). */
#define MIDI_AUTO_CLOCK  0x01
#define MIDI_AUTO_START  0x02
#define MIDI_AUTO_STOP   0x04

/* Sync source for the local Link tempo. */
typedef enum {
    MIDI_SYNC_OFF       = 0,    /* ignore incoming clock */
    MIDI_SYNC_OBSERVE   = 1,    /* measure BPM but don't change Link */
    MIDI_SYNC_FOLLOWER  = 2,    /* set Link tempo to match incoming clock */
} midi_sync_mode_t;

void     midi_init(void);
void     midi_tick(void);

/* TX */
int      midi_send_byte(uint8_t b);
void     midi_set_auto_tx(uint8_t flags);
uint8_t  midi_get_auto_tx(void);

/* RX */
int      midi_recv_byte(uint8_t *out);

/* Sync from incoming clock */
void     midi_set_sync_mode(midi_sync_mode_t m);
midi_sync_mode_t midi_get_sync_mode(void);
double   midi_observed_bpm(void);     /* 0 if no clock seen */
uint32_t midi_observed_period_us(void);
uint64_t midi_last_clock_ts_us(void);

#endif /* LINKFPGA_MIDI_H */
