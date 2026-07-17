/* Minimal eCos HAL architecture stub — compile-check only (see kapi.h). */
#ifndef ECOS_STUB_HAL_ARCH_H
#define ECOS_STUB_HAL_ARCH_H

#define CYGARC_HAL_SAVE_GP()
#define CYGARC_HAL_RESTORE_GP()
#define HAL_REORDER_BARRIER() __asm__ volatile ("" : : : "memory")

#endif /* ECOS_STUB_HAL_ARCH_H */
