/* Compile-time configuration for the LinkFPGA firmware. */

#ifndef LINKFPGA_CONFIG_H
#define LINKFPGA_CONFIG_H

#include <generated/csr.h>
#include <generated/soc.h>

/* Constants exported by litex_soc/soc.py via add_constant(). */
#ifndef LINK_NUM_TDM_PORTS
#define LINK_NUM_TDM_PORTS          2
#endif
#ifndef LINK_NUM_PHYSICAL_TDM_PORTS
#define LINK_NUM_PHYSICAL_TDM_PORTS 2
#endif
#ifndef LINK_TDM_CHANNELS_PER_PORT
#define LINK_TDM_CHANNELS_PER_PORT  16
#endif
#ifndef LINK_TDM_FS_HZ
#define LINK_TDM_FS_HZ              48000
#endif

/* Protocol constants from LINK_PROTOCOL_SPEC.md §10. */
#define LINK_DISCOVERY_TTL_SEC      5
#define LINK_DISCOVERY_BCAST_MS     250
#define LINK_DISCOVERY_MIN_MS       50
#define LINK_DISCOVERY_PRUNE_GRACE_MS 1000
#define LINK_DISCOVERY_PORT         20808
#define LINK_MCAST_V4               "224.76.78.75"
/* 224.76.78.75 packed as host-order uint32 (matches libliteeth's IPTOINT) */
#define LINK_MCAST_V4_ADDR          ((224u<<24)|(76u<<16)|(78u<<8)|75u)
#define LINK_CONTROL_PORT           20809   /* UDP control protocol */

#define LINK_MAX_PEERS              16
#define LINK_MAX_CHANNELS           (LINK_NUM_TDM_PORTS * LINK_TDM_CHANNELS_PER_PORT)

#define LINK_PING_INTERVAL_MS       50
#define LINK_MAX_PINGS              5
#define LINK_SAMPLE_THRESHOLD       100
#define LINK_SESSION_EPS_US         500000LL
#define LINK_SESSION_REMEASURE_S    30

#define LINK_MAX_MSG_DISCOVERY      512
#define LINK_MAX_MSG_LINK           512
#define LINK_MAX_MSG_AUDIO          1200

/* Default tempo (120 BPM) used until something better arrives. */
#define LINK_DEFAULT_TEMPO_US_PER_BEAT  500000LL

/* HTTP server */
#define LINK_HTTP_PORT              80
#define LINK_HTTP_MAX_REQUEST       1024

#endif /* LINKFPGA_CONFIG_H */
