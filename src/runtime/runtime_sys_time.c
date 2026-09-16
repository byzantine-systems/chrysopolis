/*
 * Clock resolution, sleeps and the CLOCK_REALTIME epoch offset.
 *
 * The sDDF timer is the only clock, and it starts near zero every boot. libc's
 * own clock_gettime handler reads it for every clock id. This unit replaces
 * that handler, chains to it, and for CLOCK_REALTIME only adds the base epoch
 * plus the per-boot offset rng_init derives. ERTS seeds erlang:make_ref() and
 * rand from gettimeofday, so the offset is what makes them differ across
 * boots. CLOCK_MONOTONIC passes through untouched.
 *
 * clock_nanosleep serves musl's nanosleep, usleep and sleep. It arms the timer
 * multiplexer for the deadline and parks the cothread on the idle semaphore,
 * rechecking the monotonic clock after each pulse. notified(timer) pulses the
 * waiters when the timeout fires, so the PD idles at ~0% CPU during a sleep.
 * notified() also pumps lwIP, so the network keeps advancing meanwhile.
 */
#include "rng.h"
#include "runtime_config.h"
#include "runtime_deadline.h"
#include "runtime_syscall_handlers.h"
#include "runtime_timer.h"
#include "runtime_wait.h"

#include <sddf/timer/client.h>

#include <errno.h>
#include <stdint.h>
#include <time.h>

static muslcsys_syscall_t previous_clock_gettime;

void runtime_sys_clock_gettime_chain(muslcsys_syscall_t previous) {
  previous_clock_gettime = previous;
}

/* ERTS queries the monotonic clock resolution. Report 1ns granularity. */
long runtime_sys_clock_getres(va_list ap) {
  (void)va_arg(ap, long); /* clockid */
  struct timespec *res = runtime_sys_arg_pointer(va_arg(ap, long));
  if (res != nullptr) {
    res->tv_sec = 0;
    res->tv_nsec = 1;
  }
  return 0;
}

long runtime_sys_clock_nanosleep(va_list ap) {
  const long clockid = va_arg(ap, long);
  const int flags = runtime_sys_arg_int(va_arg(ap, long));
  const struct timespec *req = runtime_sys_arg_pointer(va_arg(ap, long));
  struct timespec *rem = runtime_sys_arg_pointer(va_arg(ap, long));
  if (req == nullptr) {
    return -EFAULT;
  }
  if (!runtime_timespec_valid(req)) {
    return -EINVAL;
  }
  const bool absolute = flags & TIMER_ABSTIME;
  const unsigned ch = timer_config.driver_id;
  uint64_t reqns = runtime_timespec_ns(req);
  /* An absolute CLOCK_REALTIME deadline lies on the epoch-offset timeline of
   * runtime_sys_clock_gettime. The sDDF timer is monotonic, so shift such a
   * deadline back first. Unshifted it is ~56 years away and the sleep never
   * returns. Relative sleeps and monotonic deadlines pass through. */
  if (absolute && clockid == CLOCK_REALTIME) {
    const uint64_t epoch_ns =
        (RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec) *
        runtime_ns_per_sec;
    reqns = reqns > epoch_ns ? reqns - epoch_ns : 0;
  }
  const uint64_t target =
      absolute ? reqns : runtime_deadline_after(sddf_timer_time_now(ch), reqns);
  while (sddf_timer_time_now(ch) < target) {
    /* Any pulse wakes the park, the timer firing or another notification.
     * Recheck the clock and rearm for whatever time remains. */
    beam_timer_arm(target);
    thread_io_wait();
  }
  if (rem != nullptr && !absolute) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

/* The previous handler consumes a copy of the argument list, and this handler
 * reads clk_id and tp from the original to decide whether to add the offset. */
long runtime_sys_clock_gettime(va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  const long ret = previous_clock_gettime != nullptr
                       ? previous_clock_gettime(copy)
                       : -ENOSYS;
  va_end(copy);
  if (ret != 0) {
    return ret;
  }
  const clockid_t clk_id = (clockid_t)runtime_sys_arg_int(va_arg(ap, long));
  struct timespec *tp = runtime_sys_arg_pointer(va_arg(ap, long));
  if (clk_id == CLOCK_REALTIME && tp != nullptr) {
    tp->tv_sec += (time_t)(RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec);
  }
  return ret;
}
