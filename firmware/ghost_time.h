/*
 * Wrapper around the GhostTimeUnit CSRs (litex_soc/ghost_time.py).
 *
 * The hardware exposes a 1 MHz host counter and a software-loaded 64-bit
 * intercept that turns it into a session ghost time. Reading host_lo
 * latches host_hi/ghost_lo/ghost_hi atomically, so we always read host_lo
 * first.
 */

#ifndef LINKFPGA_GHOST_TIME_H
#define LINKFPGA_GHOST_TIME_H

#include <stdint.h>

/* 64-bit microsecond host clock (1 MHz). */
uint64_t host_time_us(void);

/* host_time + intercept (mod 2^64). */
uint64_t ghost_time_us(void);

/* Replace the GhostXForm intercept (signed 64-bit µs). */
void ghost_time_set_intercept(int64_t intercept);

int64_t ghost_time_get_intercept(void);

#endif /* LINKFPGA_GHOST_TIME_H */
