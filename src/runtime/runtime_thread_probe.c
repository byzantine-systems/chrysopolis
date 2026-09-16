/*
 * Diagnostic guest probe for pthread semantics ERTS boot alone cannot show.
 * Built only with -Ddiagnostic and run before the normal payload starts.
 */
#include "runtime_thread_probe.h"
#include "runtime_cothread_state.h"
#include "runtime_wait.h"

#include <libmicrokitco.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_key_t probe_key;
static int destructor_count;
static int once_calls;
static int once_observed;
static int cond_ready;
static int cond_observed;
static int detached_done;
static _Atomic(bool) slot_release;
static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;
static int probe_value;

typedef struct {
  size_t calls;
  size_t released;
  size_t fail_after;
} probe_stack_allocator;

static void *probe_stack_alloc(size_t bytes, void *context) {
  probe_stack_allocator *allocator = context;
  if (allocator->calls == allocator->fail_after) {
    return NULL;
  }
  allocator->calls++;
  return malloc(bytes);
}

static void probe_stack_free(void *pointer, void *context) {
  probe_stack_allocator *allocator = context;
  if (pointer != NULL) {
    allocator->released++;
    free(pointer);
  }
}

static bool probe_check(bool condition, const char *stage) {
  if (!condition) {
    printf("COTHREAD_PROBE|FAIL|%s\n", stage);
  }
  return condition;
}

static void probe_destructor(void *value) {
  if (value == &probe_value) {
    destructor_count++;
  }
}

static void *probe_return(void *arg) { return arg; }

static void *probe_exit(void *arg) {
  if (pthread_setspecific(probe_key, &probe_value) != 0) {
    return NULL;
  }
  pthread_exit(arg);
}

static void probe_once_init(void) {
  once_calls++;
  microkit_cothread_yield();
}

static void *probe_once_worker(void *arg) {
  (void)arg;
  if (pthread_once(&once_control, probe_once_init) == 0 && once_calls == 1) {
    once_observed++;
  }
  return NULL;
}

static void *probe_cond_waiter(void *arg) {
  (void)arg;
  if (pthread_mutex_lock(&cond_mutex) != 0) {
    return NULL;
  }
  while (!cond_ready) {
    if (pthread_cond_wait(&cond_var, &cond_mutex) != 0) {
      pthread_mutex_unlock(&cond_mutex);
      return NULL;
    }
  }
  cond_observed++;
  pthread_mutex_unlock(&cond_mutex);
  return NULL;
}

static void *probe_cond_signaler(void *arg) {
  (void)arg;
  if (pthread_mutex_lock(&cond_mutex) != 0) {
    return NULL;
  }
  cond_ready = 1;
  pthread_cond_signal(&cond_var);
  pthread_mutex_unlock(&cond_mutex);
  return NULL;
}

static void *probe_detached(void *arg) {
  (void)arg;
  detached_done = 1;
  return NULL;
}

static void *probe_slot_worker(void *arg) {
  while (!atomic_load_explicit(&slot_release, memory_order_acquire)) {
    thread_io_wait();
  }
  return arg;
}

static bool probe_stack_contract(void) {
  const size_t stack_count = LIBMICROKITCO_MAX_COTHREADS - 1;
  const size_t probe_stack_bytes = 4096;
  for (size_t fail_after = 0; fail_after < stack_count; fail_after++) {
    probe_stack_allocator allocator = {.fail_after = fail_after};
    stack_ptrs_arg_array_t stacks = {};
    if (!probe_check(!runtime_stack_prepare(
                         stacks, stack_count, probe_stack_bytes,
                         probe_stack_alloc, probe_stack_free, &allocator) &&
                         allocator.released == fail_after,
                     "stack-failure-cleanup")) {
      return false;
    }
    for (size_t i = 0; i < stack_count; i++) {
      if (!probe_check(stacks[i] == 0, "stack-failure-output")) {
        return false;
      }
    }
  }

  probe_stack_allocator invalid = {.fail_after = stack_count};
  stack_ptrs_arg_array_t untouched = {};
  for (size_t i = 0; i < stack_count; i++) {
    untouched[i] = UINTPTR_MAX;
  }
  if (!probe_check(!runtime_stack_prepare(untouched, stack_count - 1,
                                          probe_stack_bytes, probe_stack_alloc,
                                          probe_stack_free, &invalid) &&
                       !runtime_stack_prepare(untouched, stack_count, 4095,
                                              probe_stack_alloc,
                                              probe_stack_free, &invalid) &&
                       !runtime_stack_prepare(untouched, stack_count,
                                              probe_stack_bytes, NULL,
                                              probe_stack_free, &invalid) &&
                       invalid.calls == 0,
                   "stack-invalid-arguments")) {
    return false;
  }
  for (size_t i = 0; i < stack_count; i++) {
    if (!probe_check(untouched[i] == UINTPTR_MAX, "stack-invalid-output")) {
      return false;
    }
  }

  probe_stack_allocator complete = {.fail_after = stack_count};
  stack_ptrs_arg_array_t stacks = {};
  if (!probe_check(runtime_stack_prepare(stacks, stack_count, probe_stack_bytes,
                                         probe_stack_alloc, probe_stack_free,
                                         &complete),
                   "stack-success")) {
    return false;
  }
  for (size_t i = 0; i < stack_count; i++) {
    if (!probe_check(stacks[i] != 0, "stack-success-output")) {
      return false;
    }
    probe_stack_free((void *)stacks[i], &complete);
  }
  return probe_check(complete.released == stack_count, "stack-success-release");
}

static bool probe_slot_exhaustion(void) {
  pthread_t threads[LIBMICROKITCO_MAX_COTHREADS - 1] = {};
  atomic_store_explicit(&slot_release, false, memory_order_relaxed);
  for (size_t i = 0; i < LIBMICROKITCO_MAX_COTHREADS - 1; i++) {
    if (!probe_check(pthread_create(&threads[i], NULL, probe_slot_worker,
                                    &threads[i]) == 0,
                     "slot-fill")) {
      return false;
    }
  }
  pthread_t rejected = PTHREAD_NULL;
  if (!probe_check(pthread_create(&rejected, NULL, probe_slot_worker, NULL) ==
                           EAGAIN &&
                       rejected == PTHREAD_NULL,
                   "slot-exhaustion")) {
    return false;
  }
  atomic_store_explicit(&slot_release, true, memory_order_release);
  thread_io_wake();
  for (size_t i = 0; i < LIBMICROKITCO_MAX_COTHREADS - 1; i++) {
    void *result = NULL;
    if (!probe_check(pthread_join(threads[i], &result) == 0 &&
                         result == &threads[i],
                     "slot-reclaim")) {
      return false;
    }
  }
  pthread_t reused = PTHREAD_NULL;
  if (!probe_check(pthread_create(&reused, NULL, probe_return, NULL) == 0 &&
                       pthread_join(reused, NULL) == 0,
                   "slot-reuse")) {
    return false;
  }
  return true;
}

bool runtime_thread_probe_run(void) {
  if (!probe_stack_contract()) {
    return false;
  }

  pthread_attr_t attr = {};
  size_t stacksize = 0;
  if (!probe_check(pthread_attr_init(&attr) == 0 &&
                       pthread_attr_getstacksize(&attr, &stacksize) == 0 &&
                       stacksize == 512U * 1024U &&
                       pthread_attr_setstacksize(&attr, 512U * 1024U + 1) ==
                           EINVAL,
                   "stack-attr")) {
    return false;
  }
  if (!probe_check(pthread_join(PTHREAD_NULL, NULL) == ESRCH &&
                       pthread_setspecific((pthread_key_t)UINT_MAX, NULL) ==
                           EINVAL,
                   "invalid-id")) {
    return false;
  }

  pthread_key_t keys[64] = {};
  size_t key_count = 0;
  while (key_count < 64 && pthread_key_create(&keys[key_count], NULL) == 0) {
    key_count++;
  }
  pthread_key_t extra = 0;
  if (!probe_check(key_count > 0 && pthread_key_create(&extra, NULL) == EAGAIN,
                   "key-exhaustion")) {
    return false;
  }
  pthread_key_t reused = keys[key_count - 1];
  if (!probe_check(pthread_key_delete(reused) == 0 &&
                       pthread_key_create(&probe_key, probe_destructor) == 0 &&
                       probe_key == reused,
                   "key-reuse")) {
    return false;
  }
  for (size_t i = 0; i + 1 < key_count; i++) {
    pthread_key_delete(keys[i]);
  }

  pthread_mutexattr_t mutex_attr = {};
  pthread_mutex_t mutex = {};
  if (!probe_check(pthread_mutexattr_init(&mutex_attr) == 0 &&
                       pthread_mutexattr_settype(
                           &mutex_attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
                       pthread_mutex_init(&mutex, &mutex_attr) == 0 &&
                       pthread_mutex_lock(&mutex) == 0 &&
                       pthread_mutex_lock(&mutex) == 0 &&
                       pthread_mutex_unlock(&mutex) == 0 &&
                       pthread_mutex_trylock(&mutex) == 0 &&
                       pthread_mutex_unlock(&mutex) == 0 &&
                       pthread_mutex_unlock(&mutex) == 0 &&
                       pthread_mutex_destroy(&mutex) == 0,
                   "recursive-mutex")) {
    return false;
  }

  pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
  if (!probe_check(pthread_rwlock_rdlock(&rw) == 0 &&
                       pthread_rwlock_trywrlock(&rw) == EBUSY &&
                       pthread_rwlock_unlock(&rw) == 0 &&
                       pthread_rwlock_wrlock(&rw) == 0 &&
                       pthread_rwlock_tryrdlock(&rw) == EBUSY &&
                       pthread_rwlock_trywrlock(&rw) == EBUSY &&
                       pthread_rwlock_unlock(&rw) == 0 &&
                       pthread_rwlock_tryrdlock(&rw) == 0 &&
                       pthread_rwlock_unlock(&rw) == 0,
                   "rwlock-transition")) {
    return false;
  }

  pthread_t first = PTHREAD_NULL;
  void *result = NULL;
  if (!probe_check(
          pthread_create(&first, NULL, probe_return, &probe_value) == 0 &&
              pthread_join(first, &result) == 0 && result == &probe_value &&
              pthread_join(first, NULL) == ESRCH,
          "join-result")) {
    return false;
  }
  pthread_t second = PTHREAD_NULL;
  if (!probe_check(
          pthread_create(&second, NULL, probe_exit, &probe_value) == 0 &&
              pthread_join(second, &result) == 0 && result == &probe_value &&
              destructor_count == 1 && first != second,
          "exit-tls")) {
    return false;
  }
  pthread_key_delete(probe_key);

  pthread_t once_a = PTHREAD_NULL;
  pthread_t once_b = PTHREAD_NULL;
  if (!probe_check(
          pthread_create(&once_a, NULL, probe_once_worker, NULL) == 0 &&
              pthread_create(&once_b, NULL, probe_once_worker, NULL) == 0 &&
              pthread_join(once_a, NULL) == 0 &&
              pthread_join(once_b, NULL) == 0 && once_calls == 1 &&
              once_observed == 2,
          "once-completion")) {
    return false;
  }

  pthread_t waiter = PTHREAD_NULL;
  pthread_t signaler = PTHREAD_NULL;
  if (!probe_check(
          pthread_create(&waiter, NULL, probe_cond_waiter, NULL) == 0 &&
              pthread_create(&signaler, NULL, probe_cond_signaler, NULL) == 0 &&
              pthread_join(waiter, NULL) == 0 &&
              pthread_join(signaler, NULL) == 0 && cond_observed == 1,
          "cond-wake")) {
    return false;
  }

  pthread_mutex_t timed_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t timed_cond = PTHREAD_COND_INITIALIZER;
  const struct timespec expired = {.tv_sec = 0, .tv_nsec = 0};
  if (!probe_check(pthread_mutex_lock(&timed_mutex) == 0 &&
                       pthread_cond_timedwait(&timed_cond, &timed_mutex,
                                              &expired) == ETIMEDOUT &&
                       pthread_mutex_unlock(&timed_mutex) == 0,
                   "cond-expired-deadline")) {
    return false;
  }

  pthread_t detached = PTHREAD_NULL;
  if (!probe_check(
          pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
              pthread_create(&detached, &attr, probe_detached, NULL) == 0 &&
              pthread_join(detached, NULL) == EINVAL,
          "detach-active")) {
    return false;
  }
  for (size_t attempts = 0; attempts < 4096 && !detached_done; attempts++) {
    microkit_cothread_yield();
  }
  if (!probe_check(detached_done && pthread_join(detached, NULL) == ESRCH,
                   "detach-complete")) {
    return false;
  }

  if (!probe_slot_exhaustion()) {
    return false;
  }

  printf("COTHREAD_PROBE|PASS\n");
  return true;
}
