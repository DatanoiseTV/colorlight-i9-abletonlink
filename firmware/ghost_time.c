#include "ghost_time.h"
#include "config.h"

uint64_t host_time_us(void) {
    /* Read host_lo first to latch the snapshot. */
    uint32_t lo = ghost_time_host_lo_read();
    uint32_t hi = ghost_time_host_hi_read();
    return ((uint64_t)hi << 32) | lo;
}

uint64_t ghost_time_us(void) {
    /* host_lo read latches the matching ghost snapshot too, so do that
     * first. */
    (void)ghost_time_host_lo_read();
    uint32_t lo = ghost_time_ghost_lo_read();
    uint32_t hi = ghost_time_ghost_hi_read();
    return ((uint64_t)hi << 32) | lo;
}

void ghost_time_set_intercept(int64_t intercept) {
    ghost_time_inter_lo_write((uint32_t)intercept);
    ghost_time_inter_hi_write((uint32_t)(intercept >> 32));
}

int64_t ghost_time_get_intercept(void) {
    uint32_t lo = ghost_time_inter_lo_read();
    uint32_t hi = ghost_time_inter_hi_read();
    return (int64_t)(((uint64_t)hi << 32) | lo);
}
