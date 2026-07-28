/* Freestanding setjmp/longjmp for the CT952 SPARC port: map to the gcc
 * builtins, which handle SPARC register windows and need no C library.
 * Found via -I. ahead of the toolchain's <setjmp.h>. */
#ifndef CT952_SETJMP_H
#define CT952_SETJMP_H

/* gcc __builtin_setjmp needs a buffer of at least 5 words. */
typedef void *jmp_buf[5];

#define setjmp(buf)        __builtin_setjmp(buf)
/* __builtin_longjmp requires the value argument to be 1. */
#define longjmp(buf, val)  __builtin_longjmp((buf), 1)

#endif
