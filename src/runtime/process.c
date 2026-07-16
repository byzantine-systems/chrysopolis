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
 *   - locks/condvars -> yield to the cothread scheduler. ERTS wraps every
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

#include "rng.h" /* RNG_REALTIME_BASE_EPOCH, for the condvar deadline clock */

/* Timer multiplexer (main.c): keeps the single sDDF timeout slot armed for the
 * nearest pending deadline (absolute, monotonic ns). Timed waits arm it so a
 * thread_io_wait() pulse is guaranteed to arrive by their deadline. */
extern void beam_timer_arm(uint64_t deadline_ns);

/* -------------------------------------------------------------------------
 * Cothread runtime
 * ------------------------------------------------------------------------- */
#define CO_STACK_SIZE (512U * 1024U)

static co_control_t co_controller;
static int co_runtime_up;

/* Idle/wakeup plumbing for the "real blocking idle" model. ERTS now runs on a
 * spawned cothread and init() returns to the Microkit handler loop, so
 * notified() fires and can wake cothreads that parked instead of busy-yielding.
 *
 *  - io_wakeup_sem: the single semaphore every idle wait blocks on
 *    (epoll_pwait / ppoll / pselect6 / clock_nanosleep / futex / condvars).
 *    notified() pulses it on ANY channel notification (thread_io_wake), so a
 *    parked cothread re-checks its own readiness predicate and either proceeds
 *    or re-parks. A cothread blocked here is NOT "ready", so when every
 * cothread is parked the scheduler falls back to the root thread and
 * init()/notified() returns to Microkit's seL4_Recv -> the vCPU actually blocks
 * (~0% CPU).
 *
 *    A pulse must wake ALL current waiters (a serial-RX pulse must reach the
 *    epoll waiter even if a futex waiter parked first), but
 *    microkit_cothread_semaphore_signal wakes exactly one (the head). So
 *    thread_io_wake bumps a generation counter and wakes the head, and each
 *    woken waiter whose generation moved cascades the wakeup to the next
 *    waiter before returning. Waiters that re-park during the broadcast join
 *    at the TAIL with a fresh generation snapshot, so the cascade terminates.
 *  - boot_idle_sem: the root thread parks here in thread_run_cothreads() to
 *    hand control to the freshly spawned ERTS cothread. It is never signalled.
 *    When every cothread is parked the scheduler falls back to the root
 *    (internal_go_next's empty-queue fallback), which returns out of this wait
 *    and lets init() return.
 *  - park_sem: never signalled. A cothread that must block forever (ERTS's
 *    signal-dispatcher reading a pipe that never delivers, since we have no OS
 *    signals) parks here consuming no CPU instead of yielding forever. */
static microkit_cothread_sem_t io_wakeup_sem;
static microkit_cothread_sem_t boot_idle_sem;
static microkit_cothread_sem_t park_sem;
static unsigned io_wake_generation;

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
  microkit_cothread_semaphore_init(&io_wakeup_sem);
  microkit_cothread_semaphore_init(&boot_idle_sem);
  microkit_cothread_semaphore_init(&park_sem);
  co_runtime_up = 1;
}

/* Drive the cothread scheduler from the PD's notification handler: wake any
 * cothread blocked in wait_on_channel(ch) (e.g. console_read on the serial-RX
 * channel). Must be called from the root thread (recv_ntfn asserts this). */
void thread_notified(microkit_channel ch) {
  if (co_runtime_up) {
    microkit_cothread_recv_ntfn(ch);
  }
}

/* Hand control from the root thread to the spawned cothreads (ERTS + helpers)
 * and return only once they are all parked (idle). Called from init() after
 * spawning erl_start: the root parks on a never-signalled semaphore, so the
 * scheduler runs the ready cothreads until every one of them blocks, at which
 * point it falls back to the root and this returns -> init() returns to the
 * Microkit event loop. */
void thread_run_cothreads(void) {
  if (co_runtime_up) {
    microkit_cothread_semaphore_wait(&boot_idle_sem);
  }
}

/* Park the current cothread on the shared idle semaphore until the next
 * thread_io_wake() pulse (from notified() on any channel, or from a cothread
 * that changed a waited-on condition). The caller MUST re-check its own
 * predicate in a loop: pulses are broadcast, not targeted.
 *
 * The root thread must never park here (it must stay free to return to the
 * Microkit event loop, and a blocked root re-queuing itself on the semaphore
 * would corrupt its waiter list via the empty-queue root fallback), so from
 * the root this degrades to a plain yield. */
void thread_io_wait(void) {
  if (!co_runtime_up || microkit_cothread_my_handle() == 0) {
    co_yield_once();
    return;
  }
  unsigned gen = io_wake_generation;
  do {
    microkit_cothread_semaphore_wait(&io_wakeup_sem);
  } while (gen == io_wake_generation); /* stale latched pulse: re-park */
  /* Broadcast cascade: wake the next parked waiter so every waiter present at
   * the pulse re-checks. Waiters that re-parked meanwhile snapshot the new
   * generation and simply re-park, ending the cascade. */
  if (!microkit_cothread_semaphore_is_queue_empty(&io_wakeup_sem)) {
    microkit_cothread_semaphore_signal(&io_wakeup_sem);
  }
}

/* Wake every cothread parked in thread_io_wait(). Called from notified() (root)
 * on each notification and from wakers (futex FUTEX_WAKE, cond_signal, the
 * poll-wakeup pipe write). If nobody is waiting the pulse is latched by the
 * semaphore. The next waiter consumes it, sees its generation unchanged and
 * re-parks. */
void thread_io_wake(void) {
  if (co_runtime_up) {
    io_wake_generation++;
    microkit_cothread_semaphore_signal(&io_wakeup_sem);
  }
}

/* Park the current cothread forever (never woken). For blocking reads on fds
 * that will never deliver (ERTS's signal-dispatcher thread on the signal pipe:
 * there are no OS signals here). Consumes no CPU, unlike a yield loop. */
void thread_park_forever(void) {
  for (;;) {
    if (co_runtime_up) {
      microkit_cothread_semaphore_wait(&park_sem);
    } else {
      seL4_Yield();
    }
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
 * pthread_mutex (cooperative spin-yield, recursive supported)
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
 * pthread_cond: __i[0] is a signal counter. cond_wait yields for spurious
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

/* Signal/broadcast bump the counter AND pulse the idle waiters: cond waiters
 * park in thread_io_wait() (instead of the old busy-yield loop), so without the
 * pulse a signal from another cothread would never wake them. */
int pthread_cond_signal(pthread_cond_t *c) {
  FADD(&c->__u.__i[0], 1);
  thread_io_wake();
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
  FADD(&c->__u.__i[0], 1);
  thread_io_wake();
  return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  int snapshot = LOAD(&c->__u.__i[0]);
  pthread_mutex_unlock(m);
  /* Park until a pulse (cond_signal pulses, so does any notification). Bounded
   * so a wait whose signal path we somehow miss still returns spurious and
   * ERTS re-checks its predicate (it wraps every cond_wait in a loop). */
  for (int i = 0; i < 64 && LOAD(&c->__u.__i[0]) == snapshot; i++) {
    thread_io_wait();
  }
  pthread_mutex_lock(m);
  return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime) {
  int snapshot = LOAD(&c->__u.__i[0]);
  pthread_mutex_unlock(m);

  /* ERTS computes condvar deadlines on CLOCK_REALTIME (the POSIX default,    *
   * pthread_condattr_setclock below is a no-op). This was harmless while
   * LionsOS aliased CLOCK_REALTIME to CLOCK_MONOTONIC, but the RNG work gives
   * CLOCK_REALTIME a 2026 base epoch (rng.h), so a realtime deadline is
   * ~1.8e9 s while the monotonic clock sits near zero: checked against
   * CLOCK_MONOTONIC it never expires and this loop spins forever, which
   * silently wedged ERTS boot right after the erl_start handoff. Pick the
   * comparison clock by the deadline's magnitude: only a realtime-based
   * abstime can be at/after the base epoch. */
  clockid_t clk = (abstime->tv_sec >= (time_t)RNG_REALTIME_BASE_EPOCH)
                      ? CLOCK_REALTIME
                      : CLOCK_MONOTONIC;

  /* The sDDF timer lives on the monotonic timeline. Rebase a realtime deadline
   * (epoch-offset, see rng.h) before arming, exactly like clock_nanosleep. */
  uint64_t deadline_ns =
      (uint64_t)abstime->tv_sec * 1000000000ull + (uint64_t)abstime->tv_nsec;
  if (clk == CLOCK_REALTIME) {
    uint64_t epoch_ns =
        (RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec) * 1000000000ull;
    deadline_ns = deadline_ns > epoch_ns ? deadline_ns - epoch_ns : 0;
  }

  struct timespec now;
  for (;;) {
    /* Arm the timer so a wakeup pulse arrives by the deadline, then park
     * until the next pulse (instead of the old busy-yield loop). */
    beam_timer_arm(deadline_ns);
    thread_io_wait();
    if (LOAD(&c->__u.__i[0]) != snapshot) {
      break;
    }
    if (clock_gettime(clk, &now) == 0) {
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
 * pthread_t is `struct __pthread *`. We encode the cothread handle as
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
  /* Returning from the trampoline ends the cothread. If a thread calls
   * pthread_exit directly we cannot unwind its stack, so just park it by
   * yielding forever (the scheduler runs the others). */
  for (;;) {
    co_yield_once();
  }
}

/* -------------------------------------------------------------------------
 * pthread_attr (stack size honoured loosely, cothread stacks are fixed)
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
