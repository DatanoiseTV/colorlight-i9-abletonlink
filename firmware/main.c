/*
 * LinkFPGA firmware entry point.
 *
 * Boot order:
 *   1. LiteX BIOS hands control here once SDRAM + UART are up.
 *   2. We initialise the network, ghost-time, beat pulse, TDM cores,
 *      Link discovery, Link-Audio, and HTTP server in that order.
 *   3. Main loop polls the network, runs the link/session/audio ticks,
 *      and serves any due TDM frames. Everything is event-driven on top
 *      of the host-time microsecond counter; no preemptive scheduler.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ghost_time.h"
#include "beat_pulse.h"
#include "tdm.h"
#include "net.h"
#include "link.h"
#include "link_audio.h"
#include "http_server.h"

extern void webui_init(void);
extern void session_tick(void);

static void on_udp_rx(uint32_t src_addr, uint16_t src_port,
                      uint16_t dst_port,
                      const uint8_t *buf, size_t len) {
    (void)dst_port;
    if (len < 8) return;
    /* Branch on protocol header magic. */
    if (memcmp(buf, "_asdp_v\x01", 8) == 0) {
        link_handle_rx(buf, len, src_addr, src_port);
    } else if (memcmp(buf, "_link_v\x01", 8) == 0) {
        extern void measurement_handle_rx(const uint8_t *, size_t,
                                          uint32_t, uint16_t);
        extern void measurement_responder_handle_rx(const uint8_t *,
                                                    size_t,
                                                    uint32_t,
                                                    uint16_t);
        /* Try the in-flight measurement first; if it doesn't match,
         * fall through to the responder. */
        measurement_handle_rx(buf, len, src_addr, src_port);
        measurement_responder_handle_rx(buf, len, src_addr, src_port);
    } else if (memcmp(buf, "chnnlsv\x01", 8) == 0) {
        link_audio_handle_rx(buf, len, src_addr, src_port);
    }
}

int main(void) {
    /* All CSR accessors used here come from the auto-generated csr.h
     * which LiteX produces during gateware build. */
    printf("\n*** LinkFPGA — Ableton Link in hardware ***\n");
    printf("    Spec:  ../LINK_PROTOCOL_SPEC.md\n");
    printf("    TDM:   %d ports x %d channels @ %d Hz\n",
           LINK_NUM_TDM_PORTS, LINK_TDM_CHANNELS_PER_PORT, LINK_TDM_FS_HZ);
    printf("    Phys:  %d, virtual: %d\n",
           LINK_NUM_PHYSICAL_TDM_PORTS,
           LINK_NUM_TDM_PORTS - LINK_NUM_PHYSICAL_TDM_PORTS);

    net_init();
    net_set_rx_callback(on_udp_rx);

    beat_pulse_init();
    tdm_init();
    link_init();
    link_audio_init();

    http_init(LINK_HTTP_PORT);
    webui_init();

    printf("    IP:    %d.%d.%d.%d:%d\n",
           (net_local_ipv4() >> 24) & 0xff,
           (net_local_ipv4() >> 16) & 0xff,
           (net_local_ipv4() >>  8) & 0xff,
            net_local_ipv4()        & 0xff,
            LINK_DISCOVERY_PORT);
    printf("    Web:   http://<ip>/ (serial console for IP after DHCP)\n");

    /* Send our first ALIVE on entry to the loop. */
    link_broadcast_alive_now();

    while (1) {
        net_poll();
        link_tick();
        session_tick();
        link_audio_tick();
        http_tick();

        for (int port = 0; port < LINK_NUM_TDM_PORTS; port++) {
            if (tdm_frame_pending(port)) {
                link_audio_on_tdm_frame(port);
                tdm_clear_frame_pending(port);
            }
        }
    }
    return 0;
}
