#ifndef CHRYSOPOLIS_RUNTIME_FUTEX_CMD_H
#define CHRYSOPOLIS_RUNTIME_FUTEX_CMD_H 1

#include "runtime_deadline.h"

#include <stdint.h>

/*
 * Linux futex operation decoding for the cothread-aware futex handler. Pure.
 */

/* Linux futex command numbers, after masking off the option bits. */
enum : int {
  runtime_futex_op_wait = 0,
  runtime_futex_op_wake = 1,
  runtime_futex_op_wait_bitset = 9,
  runtime_futex_op_wake_bitset = 10,
};

/* Strips FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME. */
static constexpr int runtime_futex_cmd_mask = 0x7f;
static constexpr int runtime_futex_clock_realtime = 256;

typedef enum {
  runtime_futex_wait,
  runtime_futex_wake,
  /* REQUEUE, CMP_REQUEUE, WAKE_OP, the PI commands and unknown numbers. The
   * handler reports -ENOSYS for these. */
  runtime_futex_unsupported,
} runtime_futex_cmd;

/* Classify a raw futex op argument, ignoring the option bits. */
[[__nodiscard__]] static inline runtime_futex_cmd
runtime_futex_classify(int op) {
  switch (op & runtime_futex_cmd_mask) {
  case runtime_futex_op_wait:
  case runtime_futex_op_wait_bitset:
    return runtime_futex_wait;
  case runtime_futex_op_wake:
  case runtime_futex_op_wake_bitset:
    return runtime_futex_wake;
  default:
    return runtime_futex_unsupported;
  }
}

/*
 * Monotonic deadline of a timed wait. FUTEX_WAIT is relative to now_ns;
 * FUTEX_WAIT_BITSET is absolute, on the realtime timeline (epoch_sec ahead)
 * with FUTEX_CLOCK_REALTIME.
 */
[[__nodiscard__]] static inline uint64_t
runtime_futex_deadline(int op, uint64_t timeout_ns, uint64_t now_ns,
                       uint64_t epoch_sec) {
  if ((op & runtime_futex_cmd_mask) != runtime_futex_op_wait_bitset) {
    return runtime_deadline_after(now_ns, timeout_ns);
  }
  if ((op & runtime_futex_clock_realtime) != 0) {
    return runtime_realtime_to_monotonic_ns(timeout_ns, epoch_sec);
  }
  return timeout_ns;
}

#endif
