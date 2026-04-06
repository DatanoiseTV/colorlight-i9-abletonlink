/*
 * Wrapper around the per-port TDM16Core CSRs (litex_soc/tdm.py).
 *
 * Each TDM16 port appears as a `tdmN` CSR bank (N = 0..LINK_NUM_TDM_PORTS-1).
 * The wrapper hides the indirection so other firmware modules can address
 * channels by `(port, channel)`.
 */

#ifndef LINKFPGA_TDM_H
#define LINKFPGA_TDM_H

#include <stdint.h>

void     tdm_init(void);
uint16_t tdm_read_rx (int port, int channel);
void     tdm_write_tx(int port, int channel, uint16_t sample);
uint32_t tdm_frame_count(int port);

/* Returns 1 if a frame just completed (CSR-poll fallback for the IRQ). */
int      tdm_frame_pending(int port);
void     tdm_clear_frame_pending(int port);

#endif /* LINKFPGA_TDM_H */
