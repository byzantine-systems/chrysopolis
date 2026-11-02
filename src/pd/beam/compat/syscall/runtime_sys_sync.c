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
 * A timed FUTEX_WAIT parks too, with its deadline on the timer multiplexer,
 * and returns -ETIMEDOUT when it passes. Spinning on yields instead stalled
 * the waiter until an unrelated notification (each Erlang file op took
 * ~1 s).
 */
#include "rng.h"
#include "runtime_config.h"
#include "runtime_deadline.h"
#include "runtime_futex_cmd.h"
#include "runtime_syscall_handlers.h"
#include "runtime_timer.h"
#include "runtime_trace.h"
#include "runtime_wait.h"

#include <sddf/timer/client.h>

#include <errno.h>
#include <time.h>

long runtime_sys_futex(va_list ap) {
  int *uaddr = runtime_sys_arg_pointer(va_arg(ap, long));
  const int op = runtime_sys_arg_int(va_arg(ap, long));
  const int val = runtime_sys_arg_int(va_arg(ap, long));
  const struct timespec *timeout = runtime_sys_arg_pointer(va_arg(ap, long));
  const runtime_futex_cmd cmd = runtime_futex_classify(op);

  if (cmd == runtime_futex_wake) {
    runtime_trace(runtime_trace_futex_wake, (int64_t)(uintptr_t)uaddr, 0);
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
  runtime_trace(runtime_trace_futex_wait, (int64_t)(uintptr_t)uaddr,
                timeout != nullptr ? (int64_t)runtime_timespec_ns(timeout)
                                   : -1);
  if (timeout != nullptr) {
    const unsigned ch = timer_config.driver_id;
    const uint64_t target = runtime_futex_deadline(
        op, runtime_timespec_ns(timeout), sddf_timer_time_now(ch),
        RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec);
    while (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) == val) {
      if (sddf_timer_time_now(ch) >= target) {
        runtime_trace(runtime_trace_futex_timeout, (int64_t)(uintptr_t)uaddr,
                      0);
        return -ETIMEDOUT;
      }
      beam_timer_arm(target);
      thread_io_wait();
    }
    return 0;
  }
  while (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) == val) {
    thread_io_wait();
  }
  return 0;
}
