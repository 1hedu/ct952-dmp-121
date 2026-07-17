/*
 * JupiterSDK on CT952 -- base integer types
 *
 * Ported from the Jupiter SDK (bare-metal Allwinner V3s, little-endian ARM)
 * to the CheerTek CT952/CT909 DVD platform (big-endian SPARC/LEON, eCos).
 *
 * The sparc-rtems/eCos toolchain used for this firmware predates a
 * guaranteed <stdint.h>, so fixed-width types are defined here from the
 * SPARC V8 ILP32 model (char/short/int/long = 1/2/4/4 bytes).
 * Host/test builds (JUP_HOST_BUILD) use the real <stdint.h> instead.
 */
#ifndef JUP_TYPES_H
#define JUP_TYPES_H

#ifdef JUP_HOST_BUILD

#include <stdint.h>

#else /* firmware build: SPARC V8 ILP32 */

typedef unsigned char   uint8_t;
typedef signed char     int8_t;
typedef unsigned short  uint16_t;
typedef signed short    int16_t;
typedef unsigned int    uint32_t;
typedef signed int      int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

#endif /* JUP_HOST_BUILD */

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* JUP_TYPES_H */
