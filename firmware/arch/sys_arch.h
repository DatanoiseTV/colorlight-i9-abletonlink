/*
 * lwIP system port header for the LinkFPGA firmware.
 *
 * NO_SYS=1 mode (single-threaded, cooperative). The protect/unprotect
 * pair must exist as no-ops because some lwIP code paths reference it
 * even with NO_SYS=1 (e.g. the `LOCK_TCPIP_CORE` macros).
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

typedef uint32_t sys_prot_t;

#define SYS_ARCH_DECL_PROTECT(lev) sys_prot_t lev = 0
#define SYS_ARCH_PROTECT(lev)      do { (void)lev; } while (0)
#define SYS_ARCH_UNPROTECT(lev)    do { (void)lev; } while (0)

#endif /* LWIP_ARCH_SYS_ARCH_H */
