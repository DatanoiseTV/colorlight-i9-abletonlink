/*
 * Thin wrapper around the EurorackInput CSRs (litex_soc/eurorack.py).
 *
 * The HW does edge detection + timestamping + period measurement on
 * three TTL inputs (CLK / RST / RUN). The firmware:
 *
 *   - reads the latest measurements once per main loop
 *   - in EURO_SYNC_FOLLOWER mode, sets the local Link tempo from the
 *     measured CLK_IN period (taking the configured PPQN into account)
 *   - handles RST_IN edges by snapping the Link beat phase to bar 1
 *   - handles RUN_IN level by toggling local play state
 */

#ifndef LINKFPGA_EURORACK_H
#define LINKFPGA_EURORACK_H

#include <stdint.h>

typedef enum {
    EURO_SYNC_OFF      = 0,    /* ignore the inputs */
    EURO_SYNC_OBSERVE  = 1,    /* measure but don't change Link */
    EURO_SYNC_FOLLOWER = 2,    /* drive Link tempo + transport from inputs */
} euro_sync_mode_t;

/* PPQN of the incoming clock. Common modular values:
 *   1   = one pulse per quarter note
 *   2   = two pulses per quarter note (8th notes)
 *   4   = four pulses per quarter note (16th notes)
 *   24  = MIDI-style 24 PPQN
 */
void euro_init(void);
void euro_tick(void);

void              euro_set_ppqn(uint8_t ppqn);
uint8_t           euro_get_ppqn(void);

void              euro_set_sync_mode(euro_sync_mode_t m);
euro_sync_mode_t  euro_get_sync_mode(void);

double            euro_observed_bpm(void);
uint32_t          euro_observed_period_us(void);
uint32_t          euro_clk_count(void);
int               euro_run_level(void);

#endif /* LINKFPGA_EURORACK_H */
