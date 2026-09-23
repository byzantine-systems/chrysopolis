/*
 * Shared idle wait for cothreads.
 *
 * Microkit notifications coalesce. A signal to one libmicrokitco semaphore
 * waiter therefore starts a cascade through every waiter present at that
 * generation. Each waiter rechecks its own predicate after it resumes. When
 * all workers park, control returns to the root Microkit event loop.
 */
#include "runtime_wait.h"
#include "runtime_cothread.h"
#include "runtime_cothread_state.h"

#include <sel4/sel4.h>

#include <stdatomic.h>
#include <stdint.h>

static microkit_cothread_sem_t io_wakeup_sem;
static microkit_cothread_sem_t boot_idle_sem;
static microkit_cothread_sem_t park_sem;
static _Atomic(uint64_t) io_wake_generation;

void runtime_wait_init(void) {
  microkit_cothread_semaphore_init(&io_wakeup_sem);
  microkit_cothread_semaphore_init(&boot_idle_sem);
  microkit_cothread_semaphore_init(&park_sem);
  atomic_store_explicit(&io_wake_generation, 0, memory_order_relaxed);
}

void thread_run_cothreads(void) {
  if (runtime_co_ready()) {
    microkit_cothread_semaphore_wait(&boot_idle_sem);
  }
}

void thread_io_wait(void) {
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_ready() || !runtime_co_handle_valid(handle) || handle == 0) {
    runtime_co_yield_once();
    return;
  }

  const uint64_t generation =
      atomic_load_explicit(&io_wake_generation, memory_order_acquire);
  do {
    microkit_cothread_semaphore_wait(&io_wakeup_sem);
  } while (generation ==
           atomic_load_explicit(&io_wake_generation, memory_order_acquire));

  if (!microkit_cothread_semaphore_is_queue_empty(&io_wakeup_sem)) {
    microkit_cothread_semaphore_signal(&io_wakeup_sem);
  }
}

void thread_io_wake(void) {
  if (runtime_co_ready()) {
    /* Release publishes the predicate change before the semaphore signal;
     * each waiter acquires the new generation before rechecking it. */
    atomic_fetch_add_explicit(&io_wake_generation, 1, memory_order_release);
    microkit_cothread_semaphore_signal(&io_wakeup_sem);
  }
}

void thread_park_forever(void) {
  for (;;) {
    if (runtime_co_ready() && runtime_co_current() != 0) {
      microkit_cothread_semaphore_wait(&park_sem);
    } else {
      seL4_Yield();
    }
  }
}
