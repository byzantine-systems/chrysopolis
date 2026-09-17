/* runtime_deadline.h: timespec validation, capping and deadline saturation. */
#include "check.h"

#include "runtime_deadline.h"

#include <limits.h>
#include <stdint.h>
#include <time.h>

static constexpr uint64_t ns = runtime_ns_per_sec;
static constexpr uint64_t max_ns = runtime_timeout_max_ns;

/* The largest whole-second count whose nanosecond value still fits the cap. */
static constexpr uint64_t max_whole_sec =
    runtime_timeout_max_ns / runtime_ns_per_sec;
static constexpr uint64_t max_sub_ns =
    runtime_timeout_max_ns % runtime_ns_per_sec;

static struct timespec ts(time_t sec, long nsec) {
  return (struct timespec){.tv_sec = sec, .tv_nsec = nsec};
}

static void test_timespec_valid(void) {
  CHECK(runtime_timespec_valid(&(struct timespec){}));
  CHECK(runtime_timespec_valid(&(struct timespec){.tv_nsec = 999'999'999}));
  CHECK(!runtime_timespec_valid(&(struct timespec){.tv_nsec = 1'000'000'000}));
  CHECK(!runtime_timespec_valid(&(struct timespec){.tv_nsec = -1}));
  CHECK(!runtime_timespec_valid(&(struct timespec){.tv_sec = -1}));
  const struct timespec largest = ts((time_t)INT64_MAX, 999'999'999);
  CHECK(runtime_timespec_valid(&largest));
}

static void test_timespec_ns(void) {
  CHECK_EQ_U64(runtime_timespec_ns(&(struct timespec){}), 0);
  const struct timespec one = ts(1, 5);
  CHECK_EQ_U64(runtime_timespec_ns(&one), ns + 5);

  /* Exactly the cap is representable. */
  const struct timespec at_cap = ts((time_t)max_whole_sec, (long)max_sub_ns);
  CHECK_EQ_U64(runtime_timespec_ns(&at_cap), max_ns);

  /* One nanosecond, one second, or the widest time_t over the cap all clamp. */
  const struct timespec over_ns =
      ts((time_t)max_whole_sec, (long)max_sub_ns + 1);
  CHECK_EQ_U64(runtime_timespec_ns(&over_ns), max_ns);
  const struct timespec over_sec = ts((time_t)(max_whole_sec + 1), 0);
  CHECK_EQ_U64(runtime_timespec_ns(&over_sec), max_ns);
  const struct timespec widest = ts((time_t)INT64_MAX, 999'999'999);
  CHECK_EQ_U64(runtime_timespec_ns(&widest), max_ns);
}

static void test_deadline_after(void) {
  CHECK_EQ_U64(runtime_deadline_after(0, 0), 0);
  CHECK_EQ_U64(runtime_deadline_after(5, 10), 15);

  /* The timeout is capped before it is added. */
  CHECK_EQ_U64(runtime_deadline_after(0, UINT64_MAX), max_ns);
  CHECK_EQ_U64(runtime_deadline_after(1, max_ns + 1), max_ns + 1);

  /* The sum saturates instead of wrapping. */
  CHECK_EQ_U64(runtime_deadline_after(UINT64_MAX - 1, 1), UINT64_MAX);
  CHECK_EQ_U64(runtime_deadline_after(UINT64_MAX - 1, 2), UINT64_MAX);
  CHECK_EQ_U64(runtime_deadline_after(UINT64_MAX, 0), UINT64_MAX);
  CHECK_EQ_U64(runtime_deadline_after(UINT64_MAX, max_ns), UINT64_MAX);
}

static void test_timespec_is_zero(void) {
  CHECK(runtime_timespec_is_zero(&(struct timespec){}));
  CHECK(!runtime_timespec_is_zero(&(struct timespec){.tv_nsec = 1}));
  CHECK(!runtime_timespec_is_zero(&(struct timespec){.tv_sec = 1}));
}

static void test_timeout_ms_ns(void) {
  CHECK_EQ_U64(runtime_timeout_ms_ns(0), 0);
  CHECK_EQ_U64(runtime_timeout_ms_ns(-1), 0);
  CHECK_EQ_U64(runtime_timeout_ms_ns(INT_MIN), 0);
  CHECK_EQ_U64(runtime_timeout_ms_ns(1), 1'000'000);
  CHECK_EQ_U64(runtime_timeout_ms_ns(INT_MAX), (uint64_t)INT_MAX * 1'000'000);
}

/* The base epoch Chrysopolis uses, 2026-01-01T00:00:00Z. */
static constexpr uint64_t epoch = 1'767'225'600;

static void test_deadline_is_realtime(void) {
  CHECK(!runtime_deadline_is_realtime(&(struct timespec){}, epoch));
  const struct timespec below = ts((time_t)epoch - 1, 999'999'999);
  CHECK(!runtime_deadline_is_realtime(&below, epoch));
  const struct timespec at = ts((time_t)epoch, 0);
  CHECK(runtime_deadline_is_realtime(&at, epoch));
  const struct timespec widest = ts((time_t)INT64_MAX, 0);
  CHECK(runtime_deadline_is_realtime(&widest, epoch));
  CHECK(runtime_deadline_is_realtime(&(struct timespec){}, 0));
}

static void test_realtime_to_monotonic(void) {
  const uint64_t epoch_ns = epoch * ns;
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(epoch_ns + 42, epoch), 42);
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(epoch_ns + 1, epoch), 1);
  /* At or before the epoch the deadline is already due. */
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(epoch_ns, epoch), 0);
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(0, epoch), 0);
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(UINT64_MAX, 0), UINT64_MAX);

  /* The largest epoch whose nanosecond value fits, and the first that does
   * not: the latter saturates so no deadline wraps into the future. */
  const uint64_t last_fit = UINT64_MAX / ns;
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(UINT64_MAX, last_fit),
               UINT64_MAX - last_fit * ns);
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(UINT64_MAX, last_fit + 1), 0);
  CHECK_EQ_U64(runtime_realtime_to_monotonic_ns(UINT64_MAX, UINT64_MAX), 0);
}

int main(void) {
  test_timespec_valid();
  test_timespec_ns();
  test_deadline_after();
  test_timespec_is_zero();
  test_timeout_ms_ns();
  test_deadline_is_realtime();
  test_realtime_to_monotonic();
  return check_finish("deadline");
}
