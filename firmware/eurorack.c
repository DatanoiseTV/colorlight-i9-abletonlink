/*
 * Eurorack input handler — drives the local Link state from a
 * modular system's clock / reset / run signals when the user puts
 * the device in EURO_SYNC_FOLLOWER mode.
 *
 * Reads the EurorackInput CSRs (litex_soc/eurorack.py) every main
 * loop iteration. The HW does the actual edge detection and
 * microsecond timestamping; the firmware just polls and reacts.
 */

#include <stdint.h>

#include "eurorack.h"
#include "link.h"
#include "ghost_time.h"
#include "config.h"

/* CSRs from generated/csr.h. */

static euro_sync_mode_t s_mode    = EURO_SYNC_OBSERVE;
static uint8_t          s_ppqn    = 4;
static double           s_bpm     = 0;
static uint32_t         s_period  = 0;
static uint32_t         s_last_clk_count;
static uint32_t         s_last_rst_count;
static uint32_t         s_last_run_count;
static int              s_run_level;

void euro_init(void) {
    eurorack_ctrl_write(0x01);                  /* enable */
    s_mode          = EURO_SYNC_OBSERVE;
    s_ppqn          = 4;
    s_bpm           = 0;
    s_period        = 0;
    s_last_clk_count = eurorack_clk_count_read();
    s_last_rst_count = eurorack_rst_count_read();
    s_last_run_count = eurorack_run_count_read();
    s_run_level     = eurorack_status_read() & 1;
}

void euro_set_ppqn(uint8_t ppqn) {
    if (ppqn == 0) ppqn = 1;
    s_ppqn = ppqn;
}
uint8_t euro_get_ppqn(void) { return s_ppqn; }

void euro_set_sync_mode(euro_sync_mode_t m) { s_mode = m; }
euro_sync_mode_t euro_get_sync_mode(void)   { return s_mode; }

double   euro_observed_bpm(void)        { return s_bpm; }
uint32_t euro_observed_period_us(void)  { return s_period; }
uint32_t euro_clk_count(void)           { return eurorack_clk_count_read(); }
int      euro_run_level(void)           { return s_run_level; }

void euro_tick(void) {
    /* Sample HW state. */
    uint32_t clk_count = eurorack_clk_count_read();
    uint32_t rst_count = eurorack_rst_count_read();
    uint32_t run_count = eurorack_run_count_read();
    uint32_t period    = eurorack_clk_period_read();
    int      run_level = eurorack_status_read() & 1;

    /* Period → BPM (taking PPQN into account):
     *   one period_us = one pulse interval
     *   pulses-per-beat = PPQN
     *   bpm = 60_000_000 / (period_us * ppqn) */
    if (period > 0 && period != s_period) {
        s_period = period;
        s_bpm    = 60000000.0 / ((double)period * (double)s_ppqn);
    }

    /* Clock edge events */
    if (clk_count != s_last_clk_count) {
        s_last_clk_count = clk_count;
        if (s_mode == EURO_SYNC_FOLLOWER && s_bpm > 0) {
            link_set_local_tempo(s_bpm);
        }
    }

    /* Reset edge events */
    if (rst_count != s_last_rst_count) {
        s_last_rst_count = rst_count;
        if (s_mode == EURO_SYNC_FOLLOWER) {
            /* Snap the beat origin to "now". This places us at
             * beat 0 the moment the reset arrived. */
            link_self_timeline.beat_origin_microbeats = 0;
            link_self_timeline.time_origin_us_ghost   = (int64_t)ghost_time_us();
            link_broadcast_alive_now();
        }
    }

    /* Run/Stop edge events — track the level */
    if (run_count != s_last_run_count || run_level != s_run_level) {
        s_last_run_count = run_count;
        s_run_level      = run_level;
        if (s_mode == EURO_SYNC_FOLLOWER) {
            link_set_play(run_level);
        }
    }
}
