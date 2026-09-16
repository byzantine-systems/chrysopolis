/*
 * pthread condition variables over the shared cothread idle wait.
 *
 * Every notification may wake a waiter spuriously. The caller's predicate
 * loop remains authoritative. cond_timedwait keeps the realtime/monotonic
 * deadline distinction ERTS needs after Chrysopolis adds a realtime epoch.
 */
#include "rng.h"
#include "runtime_deadline.h"
#include "runtime_pthread_abi.h"
#include "runtime_timer.h"
#include "runtime_wait.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

static int cond_generation(const pthread_cond_t *cond) {
  return __atomic_load_n(&cond->__u.__i[RUNTIME_COND_GENERATION_WORD],
                         __ATOMIC_ACQUIRE);
}

static void cond_advance(pthread_cond_t *cond) {
  int old = __atomic_load_n(&cond->__u.__i[RUNTIME_COND_GENERATION_WORD],
                            __ATOMIC_RELAXED);
  for (;;) {
    int next = old == INT_MAX ? INT_MIN : old + 1;
    if (__atomic_compare_exchange_n(
            &cond->__u.__i[RUNTIME_COND_GENERATION_WORD], &old, next, false,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      return;
    }
  }
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  (void)attr;
  if (cond == NULL) {
    return EINVAL;
  }
  *cond = (pthread_cond_t){};
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
  return cond == NULL ? EINVAL : 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  if (cond == NULL) {
    return EINVAL;
  }
  cond_advance(cond);
  thread_io_wake();
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  return pthread_cond_signal(cond);
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  if (cond == NULL || mutex == NULL) {
    return EINVAL;
  }
  int generation = cond_generation(cond);
  int result = pthread_mutex_unlock(mutex);
  if (result != 0) {
    return result;
  }
  /* ERTS wraps cond_wait in a predicate loop. A bounded pulse count lets it
   * recheck even if unrelated notifications keep reaching this waiter. */
  for (size_t i = 0; i < 64 && cond_generation(cond) == generation; i++) {
    thread_io_wait();
  }
  return pthread_mutex_lock(mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
  if (cond == NULL || mutex == NULL || abstime == NULL ||
      !runtime_timespec_valid(abstime)) {
    return EINVAL;
  }
  int generation = cond_generation(cond);
  int result = pthread_mutex_unlock(mutex);
  if (result != 0) {
    return result;
  }

  /* ERTS uses CLOCK_REALTIME deadlines; older callers can still pass a
   * monotonic deadline. Only a realtime value can exceed the base epoch. */
  clockid_t clock = abstime->tv_sec >= (time_t)RNG_REALTIME_BASE_EPOCH
                        ? CLOCK_REALTIME
                        : CLOCK_MONOTONIC;
  uint64_t deadline_ns = runtime_timespec_ns(abstime);
  if (clock == CLOCK_REALTIME) {
    uint64_t epoch_ns = (RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec) *
                        runtime_ns_per_sec;
    deadline_ns = deadline_ns > epoch_ns ? deadline_ns - epoch_ns : 0;
  }

  struct timespec now = {};
  for (;;) {
    if (cond_generation(cond) != generation) {
      break;
    }
    if (clock_gettime(clock, &now) != 0) {
      /* The sDDF clock is unavailable. Preserve the condvar contract that
       * the caller owns mutex again on return, even on this failure. */
      result = pthread_mutex_lock(mutex);
      return result == 0 ? EIO : result;
    }
    if (now.tv_sec > abstime->tv_sec ||
        (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec)) {
      result = pthread_mutex_lock(mutex);
      return result == 0 ? ETIMEDOUT : result;
    }
    beam_timer_arm(deadline_ns);
    thread_io_wait();
  }
  return pthread_mutex_lock(mutex);
}

int pthread_condattr_init(pthread_condattr_t *attr) {
  if (attr == NULL) {
    return EINVAL;
  }
  *attr = (pthread_condattr_t){};
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
  return attr == NULL ? EINVAL : 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clock) {
  if (attr == NULL || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
    return EINVAL;
  }
  /* This pinned ABI's previous implementation ignores the attribute and
   * infers the clock from an absolute deadline. Keep that boot contract. */
  return 0;
}
