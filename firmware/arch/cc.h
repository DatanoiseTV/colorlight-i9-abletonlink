/*
 * lwIP compiler / architecture port header for the LinkFPGA firmware
 * (RISC-V VexRiscv, picolibc 1.8, GCC). lwIP's `arch.h` includes this
 * to learn the basic types and a couple of compiler-specific macros.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Use the standard C types — picolibc has them. */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "u"

/* lwIP host/network conversions: VexRiscv is little-endian, the
 * default lwIP routines are correct, no override needed. */
#define BYTE_ORDER LITTLE_ENDIAN

/* Pack hints — GCC-style. */
#define PACK_STRUCT_FIELD(x)  x
#define PACK_STRUCT_STRUCT    __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("Assert: %s\n", x); for(;;); } while (0)

#define LWIP_RAND() ((u32_t)0)   /* not used; we never call DNS / DHCP rand */

#endif /* LWIP_ARCH_CC_H */
