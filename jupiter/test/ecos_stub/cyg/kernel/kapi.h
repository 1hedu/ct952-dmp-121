/*
 * Minimal eCos kernel API stub — COMPILE-CHECK ONLY.
 *
 * Lets the firmware-facing Jupiter sources (jshim_ct952.c, japp.c,
 * plus the firmware headers they include) be type-checked with a
 * generic big-endian SPARC GCC in an environment that has no eCos
 * install. Never used by the real firmware build, which points at the
 * genuine eCos headers via INSTALL_DIR.
 */
#ifndef ECOS_STUB_KAPI_H
#define ECOS_STUB_KAPI_H

typedef unsigned char  cyg_uint8;
typedef unsigned short cyg_uint16;
typedef unsigned int   cyg_uint32;
typedef int            cyg_int32;
typedef unsigned long long cyg_uint64;
typedef long long      cyg_int64;

typedef unsigned long  cyg_handle_t;
typedef unsigned long  cyg_addrword_t;
typedef unsigned long  cyg_priority_t;
typedef unsigned long  cyg_vector_t;
typedef unsigned long  cyg_ucount32;
typedef cyg_uint64     cyg_tick_count_t;
typedef cyg_uint32     cyg_flag_value_t;
typedef int            cyg_bool_t;
typedef unsigned int   cyg_flag_mode_t;

typedef struct cyg_thread { int _stub[32]; } cyg_thread;
typedef struct cyg_mutex_t { int _stub[8]; } cyg_mutex_t;
typedef struct cyg_flag_t { int _stub[8]; } cyg_flag_t;
typedef struct cyg_sem_t { int _stub[8]; } cyg_sem_t;
typedef struct cyg_interrupt { int _stub[16]; } cyg_interrupt;
typedef cyg_uint32 (*cyg_ISR_t)(cyg_vector_t vector, cyg_addrword_t data);
typedef void (*cyg_DSR_t)(cyg_vector_t vector, cyg_ucount32 count,
                          cyg_addrword_t data);

void cyg_thread_create(cyg_priority_t prio, void (*entry)(cyg_addrword_t),
                       cyg_addrword_t entry_data, char *name,
                       void *stack_base, cyg_ucount32 stack_size,
                       cyg_handle_t *handle, cyg_thread *thread);
void cyg_thread_resume(cyg_handle_t thread);
void cyg_thread_suspend(cyg_handle_t thread);
void cyg_thread_kill(cyg_handle_t thread);
void cyg_thread_yield(void);
void cyg_thread_delay(cyg_tick_count_t delay);

void cyg_mutex_init(cyg_mutex_t *mutex);
cyg_bool_t cyg_mutex_lock(cyg_mutex_t *mutex);
cyg_bool_t cyg_mutex_trylock(cyg_mutex_t *mutex);
void cyg_mutex_unlock(cyg_mutex_t *mutex);

void cyg_flag_init(cyg_flag_t *flag);
void cyg_flag_destroy(cyg_flag_t *flag);
void cyg_flag_setbits(cyg_flag_t *flag, cyg_flag_value_t value);
void cyg_flag_maskbits(cyg_flag_t *flag, cyg_flag_value_t value);
cyg_flag_value_t cyg_flag_wait(cyg_flag_t *flag, cyg_flag_value_t pattern,
                               cyg_flag_mode_t mode);
cyg_flag_value_t cyg_flag_timed_wait(cyg_flag_t *flag,
                                     cyg_flag_value_t pattern,
                                     cyg_flag_mode_t mode,
                                     cyg_tick_count_t abstime);
cyg_flag_value_t cyg_flag_peek(cyg_flag_t *flag);

cyg_handle_t cyg_real_time_clock(void);
cyg_handle_t cyg_counter_create(cyg_handle_t *counter, void *ct);
cyg_tick_count_t cyg_counter_current_value(cyg_handle_t counter);
cyg_tick_count_t cyg_current_time(void);
void cyg_clock_to_counter(cyg_handle_t clock, cyg_handle_t *counter);

void cyg_interrupt_create(cyg_vector_t vector, cyg_priority_t priority,
                          cyg_addrword_t data, cyg_ISR_t *isr, cyg_DSR_t *dsr,
                          cyg_handle_t *handle, cyg_interrupt *intr);
void cyg_interrupt_attach(cyg_handle_t interrupt);
void cyg_interrupt_detach(cyg_handle_t interrupt);
void cyg_interrupt_unmask(cyg_vector_t vector);
void cyg_interrupt_mask(cyg_vector_t vector);
void cyg_interrupt_acknowledge(cyg_vector_t vector);
void cyg_interrupt_enable(void);
void cyg_interrupt_disable(void);

void cyg_alarm_create(cyg_handle_t counter, void (*alarmfn)(cyg_handle_t, cyg_addrword_t),
                      cyg_addrword_t data, cyg_handle_t *handle, void *alarm);
void cyg_alarm_initialize(cyg_handle_t alarm, cyg_tick_count_t trigger,
                          cyg_tick_count_t interval);
void cyg_alarm_enable(cyg_handle_t alarm);
void cyg_alarm_disable(cyg_handle_t alarm);

#define CYGNUM_HAL_PRI_HIGH 0

#endif /* ECOS_STUB_KAPI_H */
