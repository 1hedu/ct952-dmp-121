/* Minimal eCos HAL interrupt stub — compile-check only (see kapi.h). */
#ifndef ECOS_STUB_HAL_INTR_H
#define ECOS_STUB_HAL_INTR_H

#define HAL_DISABLE_INTERRUPTS(x)  do { (x) = 0; } while (0)
#define HAL_ENABLE_INTERRUPTS()    do { } while (0)
#define HAL_RESTORE_INTERRUPTS(x)  do { (void)(x); } while (0)
#define HAL_QUERY_INTERRUPTS(x)    do { (x) = 0; } while (0)

#define CYGNUM_HAL_INTERRUPT_9   9
#define CYGNUM_HAL_INTERRUPT_10 10
#define CYGNUM_HAL_INTERRUPT_11 11
#define CYGNUM_HAL_INTERRUPT_12 12
#define CYGNUM_HAL_INTERRUPT_13 13

#endif /* ECOS_STUB_HAL_INTR_H */
