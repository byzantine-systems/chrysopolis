#ifndef CHRYSOPOLIS_RUNTIME_COTHREAD_H
#define CHRYSOPOLIS_RUNTIME_COTHREAD_H 1

#include <microkit.h>

#include "runtime_lifecycle.h"

/*
 * Initialise the cooperative scheduler after libc_init(), which supplies the
 * 31 fixed 512 KiB stack allocations (15.5 MiB total). The scheduler owns
 * those stacks for the rest of this boot. Repeated calls after success are
 * harmless. On allocation failure this releases every stack allocated by the
 * call, leaves the scheduler uninitialised, and returns
 * RUNTIME_STATUS_COTHREAD_ALLOC.
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

#endif
