/*
 * libmicrokitco lifetime for beam_server.
 *
 * libc_init supplies malloc before thread_init. Every stack is held for the
 * rest of this boot after microkit_cothread_init accepts the complete array.
 * On a partial allocation failure the array still belongs to this function
 * and is released in reverse order. A warm PD restart restores the writable
 * segment and reinitializes libc's heap before thread_init runs again.
 */
#include "runtime_cothread.h"
#include "runtime_cothread_state.h"
#include "runtime_wait.h"

#include <microkit.h>
#include <sel4/sel4.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static co_control_t co_controller;
static _Atomic(bool) co_runtime_up;

static void *runtime_malloc_stack(size_t bytes, void *context) {
  (void)context;
  return malloc(bytes);
}

static void runtime_free_stack(void *pointer, void *context) {
  (void)context;
  free(pointer);
}

bool runtime_stack_prepare(uintptr_t *stacks, size_t count, size_t bytes,
                           runtime_stack_alloc_fn allocate,
                           runtime_stack_free_fn release, void *context) {
  if (count != LIBMICROKITCO_MAX_COTHREADS - 1) {
    return false;
  }
  return runtime_stack_prepare_n(stacks, count, bytes, allocate, release,
                                 context);
}

bool runtime_co_ready(void) {
  return atomic_load_explicit(&co_runtime_up, memory_order_acquire);
}

bool runtime_co_handle_valid(microkit_cothread_ref_t handle) {
  return handle >= 0 && handle < LIBMICROKITCO_MAX_COTHREADS;
}

microkit_cothread_ref_t runtime_co_current(void) {
  if (!runtime_co_ready()) {
    return 0;
  }
  microkit_cothread_ref_t handle = microkit_cothread_my_handle();
  return runtime_co_handle_valid(handle) ? handle : LIBMICROKITCO_NULL_HANDLE;
}

void runtime_co_yield_once(void) {
  if (runtime_co_ready()) {
    microkit_cothread_yield();
  } else {
    seL4_Yield();
  }
}

runtime_status_t thread_init(void) {
  if (runtime_co_ready()) {
    return RUNTIME_STATUS_OK;
  }

  stack_ptrs_arg_array_t stacks = {};
  const size_t stack_count = LIBMICROKITCO_MAX_COTHREADS - 1;
  static_assert(RUNTIME_CO_STACK_SIZE >= runtime_stack_min_bytes);
  if (!runtime_stack_prepare(stacks, stack_count, RUNTIME_CO_STACK_SIZE,
                             runtime_malloc_stack, runtime_free_stack, NULL)) {
    return RUNTIME_STATUS_COTHREAD_ALLOC;
  }

  microkit_cothread_init(&co_controller, RUNTIME_CO_STACK_SIZE, stacks);
  runtime_wait_init();
  atomic_store_explicit(&co_runtime_up, true, memory_order_release);
  return RUNTIME_STATUS_OK;
}

void thread_notified(microkit_channel ch) {
  if (runtime_co_ready() && ch < MICROKIT_MAX_CHANNELS) {
    microkit_cothread_recv_ntfn(ch);
  }
}
