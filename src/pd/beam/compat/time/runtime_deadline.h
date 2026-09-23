#ifndef CHRYSOPOLIS_RUNTIME_DEADLINE_H
#define CHRYSOPOLIS_RUNTIME_DEADLINE_H 1

#include <stdbool.h>
#include <stdckdint.h>
#include <stdint.h>
#include <time.h>

/*
 * Timeout arithmetic for the syscall handlers. Pure functions of their
 * arguments, safe on any thread and before libc is initialised.
 */

static constexpr uint64_t runtime_ns_per_sec = 1000000000;

/*
 * Longest relative timeout in nanoseconds, about 292 years. The sDDF timer
 * driver adds a requested timeout to its own clock without an overflow check,
 * so a larger value would wrap there and fire at once.
 */
static constexpr uint64_t runtime_timeout_max_ns = INT64_MAX;

/* True when ts has the form POSIX requires of a timeout: a non-negative
 * second count and tv_nsec in [0, 1e9). ts must be non-null. */
[[__nodiscard__]] static inline bool
runtime_timespec_valid(const struct timespec *ts) {
  return ts->tv_sec >= 0 && ts->tv_nsec >= 0 &&
         (uint64_t)ts->tv_nsec < runtime_ns_per_sec;
}

/* Nanoseconds in a valid timespec, capped at runtime_timeout_max_ns. */
[[__nodiscard__]] static inline uint64_t
runtime_timespec_ns(const struct timespec *ts) {
  /* Both fields are non-negative for a valid timespec. */
  const uint64_t sec = (uint64_t)ts->tv_sec;
  const uint64_t nsec = (uint64_t)ts->tv_nsec;
  if (sec > (runtime_timeout_max_ns - nsec) / runtime_ns_per_sec) {
    return runtime_timeout_max_ns;
  }
  return sec * runtime_ns_per_sec + nsec;
}

/* The absolute deadline timeout_ns after now_ns. The timeout is capped at
 * runtime_timeout_max_ns and the sum saturates at UINT64_MAX. */
[[__nodiscard__]] static inline uint64_t
runtime_deadline_after(uint64_t now_ns, uint64_t timeout_ns) {
  if (timeout_ns > runtime_timeout_max_ns) {
    timeout_ns = runtime_timeout_max_ns;
  }
  return timeout_ns > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + timeout_ns;
}

/* True when a valid timespec is exactly zero, the "poll without waiting"
 * timeout. ts must be non-null. */
[[__nodiscard__]] static inline bool
runtime_timespec_is_zero(const struct timespec *ts) {
  return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

/* Nanoseconds in a non-negative millisecond timeout, as epoll_pwait takes it.
 * Every int value fits: INT_MAX ms is about 2.1e15 ns. */
[[__nodiscard__]] static inline uint64_t runtime_timeout_ms_ns(int ms) {
  return ms <= 0 ? 0 : (uint64_t)ms * 1'000'000u;
}

/*
 * True when a valid absolute deadline lies on the CLOCK_REALTIME timeline,
 * judged by whether it reaches epoch_sec. CLOCK_REALTIME never reads below its
 * base epoch, and the monotonic clock stays far below it, so a smaller value
 * is a monotonic deadline. ts must be non-null and valid.
 */
[[__nodiscard__]] static inline bool
runtime_deadline_is_realtime(const struct timespec *ts, uint64_t epoch_sec) {
  /* A valid timespec has a non-negative second count. */
  return (uint64_t)ts->tv_sec >= epoch_sec;
}

/*
 * Shift an absolute CLOCK_REALTIME deadline in nanoseconds onto the monotonic
 * sDDF timeline, which starts epoch_sec seconds earlier. A deadline before the
 * epoch is already due and maps to 0. An epoch too large for nanoseconds
 * saturates, so every deadline maps to 0 instead of a wrapped value.
 */
[[__nodiscard__]] static inline uint64_t
runtime_realtime_to_monotonic_ns(uint64_t realtime_ns, uint64_t epoch_sec) {
  uint64_t epoch_ns = 0;
  if (ckd_mul(&epoch_ns, epoch_sec, runtime_ns_per_sec)) {
    epoch_ns = UINT64_MAX;
  }
  return realtime_ns > epoch_ns ? realtime_ns - epoch_ns : 0;
}

#endif
