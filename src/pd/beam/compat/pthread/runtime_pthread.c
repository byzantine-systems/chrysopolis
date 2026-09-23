/*
 * pthread lifetime over recyclable libmicrokitco handles.
 *
 * A pthread record outlives its cothread slot until joined or detached. The
 * record list and slot map are changed only between cooperative switch points
 * on beam_server's sole TCB. No list operation yields. This lets a completed
 * joinable thread retain its result after libmicrokitco reuses the slot.
 * Records therefore use the libc heap: a fixed pool capped at the 31 active
 * cothread slots would reject valid completed-but-unjoined threads. A failed
 * allocation or spawn returns EAGAIN before publishing a record or token.
 */
#include "runtime_cothread_state.h"
#include "runtime_pthread_abi.h"
#include "runtime_pthread_handle.h"
#include "runtime_pthread_tls.h"
#include "runtime_wait.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct runtime_thread_record {
  struct runtime_thread_record *next;
  void *(*start)(void *);
  void *arg;
  void *result;
  pthread_t token;
  microkit_cothread_ref_t handle;
  bool detached;
  bool join_claimed;
  _Atomic(bool) complete;
} runtime_thread_record;

static runtime_thread_record *thread_records;
static runtime_thread_record *by_handle[LIBMICROKITCO_MAX_COTHREADS];

static runtime_thread_record *find_record(pthread_t token) {
  for (runtime_thread_record *record = thread_records; record != NULL;
       record = record->next) {
    if (record->token == token) {
      return record;
    }
  }
  return NULL;
}

static void remove_record(runtime_thread_record *record) {
  runtime_thread_record **link = &thread_records;
  while (*link != NULL && *link != record) {
    link = &(*link)->next;
  }
  if (*link == record) {
    *link = record->next;
    free(record);
  }
}

static void finish_current(void *result) {
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_handle_valid(handle) || handle == 0) {
    thread_park_forever();
  }

  runtime_thread_record *record = by_handle[(size_t)handle];
  runtime_tls_finish(handle);
  by_handle[(size_t)handle] = NULL;
  if (record != NULL) {
    record->result = result;
    atomic_store_explicit(&record->complete, true, memory_order_release);
    if (record->detached) {
      remove_record(record);
    }
  }
  thread_io_wake();
}

static void cothread_trampoline(void) {
  runtime_thread_record *record = microkit_cothread_my_arg();
  void *result = record->start(record->arg);
  finish_current(result);
  /* libmicrokitco destroys the current cothread when this entry returns. */
}

pthread_t pthread_self(void) {
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_handle_valid(handle)) {
    return PTHREAD_NULL;
  }
  if (handle == 0) {
    return runtime_thread_root_token();
  }
  runtime_thread_record *record = by_handle[(size_t)handle];
  return record != NULL ? record->token : (pthread_t)(uintptr_t)(handle + 1);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
  if (thread == NULL || start == NULL) {
    return EINVAL;
  }
  if (!runtime_co_ready()) {
    return EAGAIN;
  }
  bool detached = false;
  if (attr != NULL) {
    int state = attr->__u.__i[RUNTIME_ATTR_DETACH_INDEX];
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) {
      return EINVAL;
    }
    if (attr->__u.__s[RUNTIME_ATTR_STACK_WORD] > RUNTIME_CO_STACK_SIZE) {
      return EINVAL;
    }
    detached = state == PTHREAD_CREATE_DETACHED;
  }

  runtime_thread_record *record = malloc(sizeof(*record));
  if (record == NULL) {
    return EAGAIN;
  }
  *record =
      (runtime_thread_record){.start = start, .arg = arg, .detached = detached};
  if (!runtime_thread_token_next(&record->token)) {
    free(record);
    return EAGAIN;
  }

  microkit_cothread_ref_t handle =
      microkit_cothread_spawn(cothread_trampoline, record);
  if (handle == LIBMICROKITCO_NULL_HANDLE) {
    free(record);
    return EAGAIN;
  }
  if (!runtime_co_handle_valid(handle) || handle == 0 ||
      by_handle[(size_t)handle] != NULL) {
    /* A malformed library result is an internal contract failure. The newly
     * queued cothread owns record, so returning would leave a live dangling
     * pointer. Terminate the PD and let Root restart it. */
    abort();
  }

  record->handle = handle;
  record->next = thread_records;
  thread_records = record;
  by_handle[(size_t)handle] = record;
  runtime_tls_clear(handle);
  *thread = record->token;
  return 0;
}

int pthread_join(pthread_t thread, void **retval) {
  if (thread == PTHREAD_NULL || runtime_thread_token_is_root(thread)) {
    return ESRCH;
  }
  runtime_thread_record *record = find_record(thread);
  if (record == NULL) {
    return ESRCH;
  }
  if (thread == pthread_self()) {
    return EDEADLK;
  }
  if (record->detached || record->join_claimed) {
    return EINVAL;
  }
  record->join_claimed = true;
  while (!atomic_load_explicit(&record->complete, memory_order_acquire)) {
    thread_io_wait();
  }
  if (retval != NULL) {
    *retval = record->result;
  }
  remove_record(record);
  return 0;
}

int pthread_detach(pthread_t thread) {
  runtime_thread_record *record = find_record(thread);
  if (record == NULL) {
    return ESRCH;
  }
  if (record->detached || record->join_claimed) {
    return EINVAL;
  }
  record->detached = true;
  if (atomic_load_explicit(&record->complete, memory_order_acquire)) {
    remove_record(record);
  }
  return 0;
}

void pthread_exit(void *retval) {
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_handle_valid(handle) || handle == 0) {
    thread_park_forever();
  }
  finish_current(retval);
  microkit_cothread_destroy(handle);
  thread_park_forever();
}
