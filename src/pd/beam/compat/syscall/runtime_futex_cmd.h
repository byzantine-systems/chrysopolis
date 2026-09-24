#ifndef CHRYSOPOLIS_RUNTIME_FUTEX_CMD_H
#define CHRYSOPOLIS_RUNTIME_FUTEX_CMD_H 1

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

#endif
