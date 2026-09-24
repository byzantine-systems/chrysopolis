/*
 * Cooperative pthread locks over the pinned musl object ABI.
 *
 * musl declares these objects as ordinary int arrays and scalar ints. Casting
 * one of those fields to _Atomic(int) would not create an atomic object and
 * would violate C's effective-type rules. Direct __atomic operations preserve
 * their declared storage and make each memory order visible at its call site.
 * Runtime-owned shared state uses declared _Atomic objects in runtime_wait.c.
 * All lock users still execute on one Microkit TCB; these orders document the
 * publication edges and keep the operations correct if a cothread yields.
 */
#include "runtime_cothread_state.h"
#include "runtime_pthread_abi.h"
#include "runtime_wait.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>

static int current_owner(void) {
  microkit_cothread_ref_t handle = runtime_co_current();
  return runtime_co_handle_valid(handle) ? handle + 1 : 0;
}

/* Mutex __i[0] is owner+1, __i[1] is recursion depth, __i[3] is type. */
static int mutex_try_acquire(pthread_mutex_t *mutex) {
  int owner = current_owner();
  if (owner == 0) {
    return EINVAL;
  }
  int expected = 0;
  if (__atomic_compare_exchange_n(&mutex->__u.__i[RUNTIME_MUTEX_OWNER_WORD],
                                  &expected, owner, false, __ATOMIC_ACQUIRE,
                                  __ATOMIC_RELAXED)) {
    mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD] = 1;
    return 0;
  }
  if (expected == owner) {
    int type = mutex->__u.__i[RUNTIME_MUTEX_TYPE_WORD];
    if (type == PTHREAD_MUTEX_RECURSIVE) {
      if (mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD] == INT_MAX) {
        return EAGAIN;
      }
      mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD]++;
      return 0;
    }
    if (type == PTHREAD_MUTEX_ERRORCHECK) {
      return EDEADLK;
    }
  }
  return EBUSY;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr) {
  if (mutex == NULL) {
    return EINVAL;
  }
  int type = attr == NULL ? PTHREAD_MUTEX_NORMAL : (int)(attr->__attr & 0xf);
  if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
    return EINVAL;
  }
  *mutex = (pthread_mutex_t){};
  mutex->__u.__i[RUNTIME_MUTEX_TYPE_WORD] = type;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  if (mutex == NULL) {
    return EINVAL;
  }
  return __atomic_load_n(&mutex->__u.__i[RUNTIME_MUTEX_OWNER_WORD],
                         __ATOMIC_RELAXED) == 0
             ? 0
             : EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (mutex == NULL) {
    return EINVAL;
  }
  for (;;) {
    int result = mutex_try_acquire(mutex);
    if (result != EBUSY) {
      return result;
    }
    runtime_co_yield_once();
  }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  if (mutex == NULL) {
    return EINVAL;
  }
  int result = mutex_try_acquire(mutex);
  return result == EDEADLK ? EBUSY : result;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (mutex == NULL) {
    return EINVAL;
  }
  int owner = current_owner();
  if (owner == 0 || __atomic_load_n(&mutex->__u.__i[RUNTIME_MUTEX_OWNER_WORD],
                                    __ATOMIC_RELAXED) != owner) {
    return EPERM;
  }
  int depth = mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD];
  if (depth <= 0) {
    return EINVAL;
  }
  if (depth > 1) {
    mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD] = depth - 1;
  } else {
    mutex->__u.__i[RUNTIME_MUTEX_DEPTH_WORD] = 0;
    __atomic_store_n(&mutex->__u.__i[RUNTIME_MUTEX_OWNER_WORD], 0,
                     __ATOMIC_RELEASE);
  }
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  if (attr == NULL) {
    return EINVAL;
  }
  *attr = (pthread_mutexattr_t){};
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  return attr == NULL ? EINVAL : 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  if (attr == NULL || type < PTHREAD_MUTEX_NORMAL ||
      type > PTHREAD_MUTEX_ERRORCHECK) {
    return EINVAL;
  }
  attr->__attr = (attr->__attr & ~3u) | (unsigned)type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
  if (attr == NULL || type == NULL) {
    return EINVAL;
  }
  *type = (int)(attr->__attr & 3u);
  return 0;
}

/* One word has states -1 (writer), 0 (free), and 1..INT_MAX (readers).
 * A compare-exchange claims the whole transition, so a failed writer never
 * leaves a flag set and a reader cannot enter after a writer claims it. */
static int rw_try_read(pthread_rwlock_t *rw) {
  int expected =
      __atomic_load_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD], __ATOMIC_RELAXED);
  for (;;) {
    if (expected < 0) {
      return EBUSY;
    }
    if (expected == INT_MAX) {
      return EAGAIN;
    }
    if (__atomic_compare_exchange_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD],
                                    &expected, expected + 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return 0;
    }
  }
}

static int rw_try_write(pthread_rwlock_t *rw) {
  int expected = 0;
  if (!__atomic_compare_exchange_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD],
                                   &expected, -1, false, __ATOMIC_ACQUIRE,
                                   __ATOMIC_RELAXED)) {
    return EBUSY;
  }
  rw->__u.__i[RUNTIME_RW_WRITER_WORD] = current_owner();
  return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *rw,
                        const pthread_rwlockattr_t *attr) {
  (void)attr;
  if (rw == NULL) {
    return EINVAL;
  }
  *rw = (pthread_rwlock_t){};
  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
  if (rw == NULL) {
    return EINVAL;
  }
  return __atomic_load_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD],
                         __ATOMIC_RELAXED) == 0
             ? 0
             : EBUSY;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
  if (rw == NULL) {
    return EINVAL;
  }
  for (;;) {
    int result = rw_try_read(rw);
    if (result != EBUSY) {
      return result;
    }
    runtime_co_yield_once();
  }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw) {
  return rw == NULL ? EINVAL : rw_try_read(rw);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
  if (rw == NULL) {
    return EINVAL;
  }
  for (;;) {
    int result = rw_try_write(rw);
    if (result != EBUSY) {
      return result;
    }
    runtime_co_yield_once();
  }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw) {
  return rw == NULL ? EINVAL : rw_try_write(rw);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
  if (rw == NULL) {
    return EINVAL;
  }
  int state =
      __atomic_load_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD], __ATOMIC_RELAXED);
  if (state == -1) {
    if (rw->__u.__i[RUNTIME_RW_WRITER_WORD] != current_owner()) {
      return EPERM;
    }
    rw->__u.__i[RUNTIME_RW_WRITER_WORD] = 0;
    __atomic_store_n(&rw->__u.__i[RUNTIME_RW_STATE_WORD], 0, __ATOMIC_RELEASE);
    return 0;
  }
  if (state <= 0) {
    return EPERM;
  }
  __atomic_fetch_sub(&rw->__u.__i[RUNTIME_RW_STATE_WORD], 1, __ATOMIC_RELEASE);
  return 0;
}

int pthread_once(pthread_once_t *control, void (*init_routine)(void)) {
  if (control == NULL || init_routine == NULL) {
    return EINVAL;
  }
  for (;;) {
    int state = __atomic_load_n(control, __ATOMIC_ACQUIRE);
    if (state == 2) {
      return 0;
    }
    if (state == 0) {
      int expected = 0;
      if (__atomic_compare_exchange_n(control, &expected, 1, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        init_routine();
        __atomic_store_n(control, 2, __ATOMIC_RELEASE);
        thread_io_wake();
        return 0;
      }
    } else if (state == 1) {
      thread_io_wait();
    } else {
      return EINVAL;
    }
  }
}

int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
  if (lock == NULL || pshared != PTHREAD_PROCESS_PRIVATE) {
    return EINVAL;
  }
  *lock = 0;
  return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
  if (lock == NULL) {
    return EINVAL;
  }
  return __atomic_load_n(lock, __ATOMIC_RELAXED) == 0 ? 0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
  if (lock == NULL) {
    return EINVAL;
  }
  for (;;) {
    int expected = 0;
    if (__atomic_compare_exchange_n(lock, &expected, 1, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      return 0;
    }
    runtime_co_yield_once();
  }
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
  if (lock == NULL) {
    return EINVAL;
  }
  int expected = 0;
  return __atomic_compare_exchange_n(lock, &expected, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
             ? 0
             : EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
  if (lock == NULL) {
    return EINVAL;
  }
  if (__atomic_load_n(lock, __ATOMIC_RELAXED) == 0) {
    return EPERM;
  }
  __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
  return 0;
}
