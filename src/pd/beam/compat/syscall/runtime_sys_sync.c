/*
 * Cothread-aware futex.
 *
 * ERTS's low-level thread-event primitive (ethr_event) calls futex directly
 * and treats -ENOSYS as fatal. FUTEX_WAIT parks the calling cothread on the
 * shared idle semaphore (thread_io_wait) until the word changes. Cothreads are
 * cooperative, nothing runs between the compare and the park, so no wakeup can
 * be lost there. FUTEX_WAKE pulses the idle semaphore (thread_io_wake) and
 * every parked waiter re-checks its word. notified() pulses wake waiters
 * spuriously too. They re-compare and park again, so the PD idles at ~0% CPU
 * here.
 *
 * A timed FUTEX_WAIT keeps a bounded yield loop. Its deadline must be honoured,
 * and mapping arbitrary futex timeouts onto the sDDF timer would cost more
 * than it saves: timed waits happen during active work, and the idle path is
 * the untimed wait.
 */
#include "runtime_deadline.h"
#include "runtime_futex_cmd.h"
#include "runtime_syscall_handlers.h"
#include "runtime_wait.h"

#include <libmicrokitco.h>

#include <errno.h>
#include <time.h>

static constexpr int timed_wait_yields = 4096;

long runtime_sys_futex(va_list ap) {
  int *uaddr = runtime_sys_arg_pointer(va_arg(ap, long));
  const int op = runtime_sys_arg_int(va_arg(ap, long));
  const int val = runtime_sys_arg_int(va_arg(ap, long));
  const struct timespec *timeout = runtime_sys_arg_pointer(va_arg(ap, long));
  const runtime_futex_cmd cmd = runtime_futex_classify(op);

  if (cmd == runtime_futex_wake) {
    thread_io_wake();
    return 0;
  }
  /* ethr_event and musl's __wait/__wake use only WAIT and WAKE. */
  if (cmd == runtime_futex_unsupported) {
    return -ENOSYS;
  }
  if (uaddr == nullptr) {
    return -EFAULT;
  }
  if (timeout != nullptr && !runtime_timespec_valid(timeout)) {
    return -EINVAL;
  }
  if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
    return -EAGAIN;
  }
  if (timeout != nullptr) {
    /* Return spuriously after a bounded number of yields. ERTS re-checks its
     * condition and re-arms the wait. */
    for (int i = 0; i < timed_wait_yields; i++) {
      microkit_cothread_yield();
      if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        return 0;
      }
    }
    return 0;
  }
  while (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) == val) {
    thread_io_wait();
  }
  return 0;
}
