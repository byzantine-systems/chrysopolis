/*
 * Thread layer: the pthread ABI ERTS needs, implemented over libmicrokitco
 *
 * ERTS requires shared-address-space threads (an aux thread, poll thread and
 * dirty-I/O schedulers) and aborts if thread creation fails. Microkit is
 * strictly one TCB per VSpace. Since `+S 1:1` needs no real parallelism, we
 * run ERTS's helper threads as cothreads in the single beam_server PD:
 *
 *   - pthread_create -> microkit_cothread_spawn
 *   - pthread_self   -> the current cothread handle
 *   - errno + pthread_key TLS -> indexed by the current cothread handle
 *     (ERTS uses pthread_key, never native __thread, so no TPIDR juggling)
 *   - locks/condvars -> yield to the cothread scheduler; ERTS wraps every
 *     cond_wait in a condition loop, so spurious wakeups are fine
 *
 * Cooperative scheduling: a cothread runs until it yields/blocks. ERTS's
 * threads coordinate via condvars (= yield points), so boot makes progress.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <microkit.h>
#include <sel4/sel4.h>

#include <libmicrokitco.h>

/* -------------------------------------------------------------------------
 * Cothread runtime
 * ------------------------------------------------------------------------- */
#define CO_STACK_SIZE (512U * 1024U)

static co_control_t co_controller;
static int co_runtime_up;

/* Per-cothread pthread_key storage, indexed by cothread handle. errno is
 * left to musl (its single main-thread errno is safe here: cothreads only
 * yield at explicit pthread waits, never between a syscall and its errno
 * check). */
#define MAX_KEYS 64
static void *co_tsd[LIBMICROKITCO_MAX_COTHREADS][MAX_KEYS];

static inline microkit_cothread_ref_t cur_handle(void) {
  if (!co_runtime_up) {
    return 0;
  }
  return microkit_cothread_my_handle();
}

/* Yield to the cothread scheduler once it exists, else to seL4 directly. */
static inline void co_yield_once(void) {
  if (co_runtime_up) {
    microkit_cothread_yield();
  } else {
    seL4_Yield();
  }
}

/* Set up the cothread runtime. Called from main() after libc_init (malloc is
 * needed for the per-cothread stacks) and before erl_start. */
void thread_init(void) {
  stack_ptrs_arg_array_t stacks;
  for (int i = 0; i < LIBMICROKITCO_MAX_COTHREADS - 1; i++) {
    stacks[i] = (uintptr_t)malloc(CO_STACK_SIZE);
  }
  microkit_cothread_init(&co_controller, CO_STACK_SIZE, stacks);
  co_runtime_up = 1;
}

/* Drive the cothread scheduler from the PD's notification handler. */
void thread_notified(microkit_channel ch) {
  if (co_runtime_up) {
    microkit_cothread_recv_ntfn(ch);
  }
}

/* -------------------------------------------------------------------------
 * Atomic helpers
 * ------------------------------------------------------------------------- */
#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define XCHG(p, v) __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)
#define FADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)
#define FSUB(p, v) __atomic_fetch_sub((p), (v), __ATOMIC_ACQ_REL)

/* -------------------------------------------------------------------------
 * pthread_mutex (cooperative spin-yield; recursive supported)
 * ------------------------------------------------------------------------- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
  for (int i = 0; i < 10; i++) {
    m->__u.__i[i] = 0;
  }
  if (attr) {
    m->__u.__i[3] = (int)(attr->__attr & 0xf);
  }
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
  (void)m;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
  int *flag = &m->__u.__i[0];
  if (m->__u.__i[3] == PTHREAD_MUTEX_RECURSIVE) {
    FADD(flag, 1);
    return 0;
  }
  while (XCHG(flag, 1) != 0) {
    co_yield_once();
  }
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
  int *flag = &m->__u.__i[0];
  if (m->__u.__i[3] == PTHREAD_MUTEX_RECURSIVE) {
    FADD(flag, 1);
    return 0;
  }
  return XCHG(flag, 1) == 0 ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
  int *flag = &m->__u.__i[0];
  if (m->__u.__i[3] == PTHREAD_MUTEX_RECURSIVE) {
    if (LOAD(flag) > 0) {
      FSUB(flag, 1);
    }
    return 0;
  }
  STORE(flag, 0);
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  attr->__attr = 0;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  (void)attr;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  if ((unsigned)type > 2) {
    return EINVAL;
  }
  attr->__attr = (attr->__attr & ~3u) | (unsigned)type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
  *type = (int)(attr->__attr & 3u);
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread_cond: __i[0] is a signal counter; cond_wait yields for spurious
 * wakeups (ERTS re-checks its predicate), letting other cothreads run.
 * ------------------------------------------------------------------------- */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
  (void)attr;
  for (int i = 0; i < 12; i++) {
    c->__u.__i[i] = 0;
  }
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
  (void)c;
  return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
  FADD(&c->__u.__i[0], 1);
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
  FADD(&c->__u.__i[0], 1);
  return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  int snapshot = LOAD(&c->__u.__i[0]);
  pthread_mutex_unlock(m);
  /* Yield to let the signaller (another cothread) run. Bounded so a wait for
   * an event that only arrives via a Microkit notification still returns
   * (spurious) and ERTS re-polls. */
  for (int i = 0; i < 64 && LOAD(&c->__u.__i[0]) == snapshot; i++) {
    co_yield_once();
  }
  pthread_mutex_lock(m);
  return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime) {
  int snapshot = LOAD(&c->__u.__i[0]);
  pthread_mutex_unlock(m);

  struct timespec now;
  for (;;) {
    co_yield_once();
    if (LOAD(&c->__u.__i[0]) != snapshot) {
      break;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
      if (now.tv_sec > abstime->tv_sec ||
          (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec)) {
        pthread_mutex_lock(m);
        return ETIMEDOUT;
      }
    }
  }

  pthread_mutex_lock(m);
  return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
  attr->__attr = 0;
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
  (void)attr;
  return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clk) {
  (void)attr;
  (void)clk;
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread_rwlock
 *   __i[0]: writer flag, __i[1]: reader count
 * ------------------------------------------------------------------------- */
int pthread_rwlock_init(pthread_rwlock_t *rw,
                        const pthread_rwlockattr_t *attr) {
  (void)attr;
  for (int i = 0; i < 14; i++) {
    rw->__u.__i[i] = 0;
  }
  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
  (void)rw;
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
  while (LOAD(&rw->__u.__i[0]) != 0) {
    co_yield_once();
  }
  FADD(&rw->__u.__i[1], 1);
  return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw) {
  if (LOAD(&rw->__u.__i[0]) != 0) {
    return EBUSY;
  }
  FADD(&rw->__u.__i[1], 1);
  return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
  while (XCHG(&rw->__u.__i[0], 1) != 0 || LOAD(&rw->__u.__i[1]) != 0) {
    co_yield_once();
  }
  return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw) {
  if (XCHG(&rw->__u.__i[0], 1) != 0 || LOAD(&rw->__u.__i[1]) != 0) {
    STORE(&rw->__u.__i[0], 0);
    return EBUSY;
  }
  return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
  if (LOAD(&rw->__u.__i[0])) {
    STORE(&rw->__u.__i[0], 0);
  } else if (LOAD(&rw->__u.__i[1]) > 0) {
    FSUB(&rw->__u.__i[1], 1);
  }
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread_key: per-cothread thread-specific data
 * ------------------------------------------------------------------------- */
static struct {
  void (*destructor)(void *);
  int used;
} key_table[MAX_KEYS];

static unsigned next_key;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  if (next_key >= MAX_KEYS) {
    return ENOMEM;
  }
  *key = next_key;
  key_table[next_key].destructor = destructor;
  key_table[next_key].used = 1;
  for (int t = 0; t < LIBMICROKITCO_MAX_COTHREADS; t++) {
    co_tsd[t][next_key] = NULL;
  }
  next_key++;
  return 0;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= MAX_KEYS || !key_table[key].used) {
    return EINVAL;
  }
  key_table[key].used = 0;
  key_table[key].destructor = NULL;
  return 0;
}

void *pthread_getspecific(pthread_key_t key) {
  if (key >= MAX_KEYS || !key_table[key].used) {
    return NULL;
  }
  return co_tsd[cur_handle()][key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= MAX_KEYS || !key_table[key].used) {
    return EINVAL;
  }
  co_tsd[cur_handle()][key] = (void *)value;
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread_once
 * ------------------------------------------------------------------------- */
int pthread_once(pthread_once_t *control, void (*init_routine)(void)) {
  if (__atomic_exchange_n(control, 1, __ATOMIC_ACQ_REL) == 0) {
    init_routine();
  }
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread identity, create, join (over cothreads).
 * pthread_t is `struct __pthread *`; we encode the cothread handle as
 * (handle + 1) so the main cothread (handle 0) is a non-NULL pthread_t.
 * ------------------------------------------------------------------------- */
#define HANDLE_TO_PTHREAD(h) ((pthread_t)(uintptr_t)((h) + 1))
#define PTHREAD_TO_HANDLE(t) ((microkit_cothread_ref_t)((uintptr_t)(t) - 1))

typedef struct {
  void *(*start)(void *);
  void *arg;
} co_start_t;

static void cothread_trampoline(void) {
  co_start_t *s = microkit_cothread_my_arg();
  s->start(s->arg);
  free(s);
}

pthread_t pthread_self(void) { return HANDLE_TO_PTHREAD(cur_handle()); }

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
  (void)attr;
  if (!co_runtime_up) {
    return EAGAIN;
  }
  co_start_t *s = malloc(sizeof(*s));
  if (s == NULL) {
    return EAGAIN;
  }
  s->start = start;
  s->arg = arg;
  microkit_cothread_ref_t h = microkit_cothread_spawn(cothread_trampoline, s);
  if (h == LIBMICROKITCO_NULL_HANDLE) {
    free(s);
    return EAGAIN;
  }
  if (thread) {
    *thread = HANDLE_TO_PTHREAD(h);
  }
  return 0;
}

int pthread_join(pthread_t thread, void **retval) {
  microkit_cothread_ref_t h = PTHREAD_TO_HANDLE(thread);
  while (co_runtime_up &&
         microkit_cothread_query_state(h) != cothread_not_active) {
    co_yield_once();
  }
  if (retval) {
    *retval = NULL;
  }
  return 0;
}

int pthread_detach(pthread_t thread) {
  (void)thread;
  return 0;
}

void pthread_exit(void *retval) {
  (void)retval;
  /* Returning from the trampoline ends the cothread; if a thread calls
   * pthread_exit directly we cannot unwind its stack, so just park it by
   * yielding forever (the scheduler runs the others). */
  for (;;) {
    co_yield_once();
  }
}

/* -------------------------------------------------------------------------
 * pthread_attr (stack size honoured loosely; cothread stacks are fixed)
 * ------------------------------------------------------------------------- */
int pthread_attr_init(pthread_attr_t *attr) {
  for (int i = 0; i < 14; i++) {
    attr->__u.__i[i] = 0;
  }
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
  (void)attr;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
  (void)attr;
  (void)stacksize;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
  (void)attr;
  *stacksize = CO_STACK_SIZE;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
  (void)attr;
  (void)detachstate;
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
  (void)attr;
  *detachstate = PTHREAD_CREATE_JOINABLE;
  return 0;
}

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr) {
  (void)attr;
  (void)stackaddr;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize) {
  (void)attr;
  (void)guardsize;
  return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
  (void)attr;
  (void)policy;
  return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
  (void)attr;
  (void)inheritsched;
  return 0;
}

int pthread_sigmask(int how, const void *set, void *oldset) {
  (void)how;
  (void)set;
  (void)oldset;
  return 0;
}

/* -------------------------------------------------------------------------
 * pthread_spin
 * ------------------------------------------------------------------------- */
int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
  (void)pshared;
  *lock = 0;
  return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
  (void)lock;
  return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
  while (XCHG(lock, 1) != 0) {
    co_yield_once();
  }
  return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
  return XCHG(lock, 1) == 0 ? 0 : EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
  STORE(lock, 0);
  return 0;
}
