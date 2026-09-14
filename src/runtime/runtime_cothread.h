#ifndef CHRYSOPOLIS_RUNTIME_COTHREAD_H
#define CHRYSOPOLIS_RUNTIME_COTHREAD_H 1

#include <microkit.h>

#include "runtime_lifecycle.h"

#include <pthread.h>

/*
 * Initialise the cooperative scheduler after libc_init(), which supplies the
 * stack allocations. The scheduler owns those stacks for the rest of this
 * boot. On allocation failure this releases every stack allocated by the call
 * and leaves the scheduler uninitialised.
 */
[[nodiscard]] runtime_status_t thread_init(void);

/*
 * Deliver one Microkit notification to cothreads waiting on ch. This must run
 * on the root thread from a Microkit callback and does not block.
 */
void thread_notified(microkit_channel ch);

/*
 * Hand execution from the root thread to ready cothreads. It returns when all
 * cothreads are parked and the root context must return to Microkit.
 */
void thread_run_cothreads(void);

/*
 * Legacy pthread entry points required by ERTS but not declared by this musl
 * pthread.h configuration. They preserve the existing ABI-compatible no-op
 * behavior and do not take ownership of pointer arguments.
 */
int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr);
int pthread_sigmask(int how, const void *set, void *oldset);

#endif
