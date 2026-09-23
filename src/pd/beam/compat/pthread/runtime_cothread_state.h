#ifndef CHRYSOPOLIS_RUNTIME_COTHREAD_STATE_H
#define CHRYSOPOLIS_RUNTIME_COTHREAD_STATE_H 1

#include "runtime_stack.h"

#include <libmicrokitco.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private scheduler interface for the single Microkit TCB. Handle zero is the
 * root context; LIBMICROKITCO_NULL_HANDLE is invalid. None of these calls
 * creates a kernel thread or shares state with another protection domain. */
enum { RUNTIME_CO_STACK_SIZE = 512U * 1024U };

/* True only after the scheduler and wait semaphores have been initialized. */
[[nodiscard]] bool runtime_co_ready(void);
/* Range check only: a valid number need not name a currently live cothread. */
[[nodiscard]] bool runtime_co_handle_valid(microkit_cothread_ref_t handle);
/* Returns root (zero) before initialization, or NULL_HANDLE on a bad handle. */
[[nodiscard]] microkit_cothread_ref_t runtime_co_current(void);
/* Yields to the cooperative scheduler after initialization, seL4 before it. */
void runtime_co_yield_once(void);

/* runtime_stack_prepare_n (runtime_stack.h) with count fixed at exactly
 * LIBMICROKITCO_MAX_COTHREADS - 1. Any other count is an invalid argument and
 * returns false without calling either callback or writing stacks. On
 * success, the caller owns every allocation until it passes the complete
 * array to microkit_cothread_init. */
[[nodiscard]] bool runtime_stack_prepare(uintptr_t *stacks, size_t count,
                                         size_t bytes,
                                         runtime_stack_alloc_fn allocate,
                                         runtime_stack_free_fn release,
                                         void *context);

#endif
