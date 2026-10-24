#ifndef CHRYSOPOLIS_RUNTIME_CTL_DEVICE_H
#define CHRYSOPOLIS_RUNTIME_CTL_DEVICE_H 1

/*
 * Pure decisions for /dev/chrysoctl and /dev/chrysostat. The adapter
 * (runtime_control_dev.c) owns storage, the PPC and errno mapping.
 *
 * Command owner transitions (any other cell keeps the state):
 *
 *   phase \ event | open    | write     | reply         | read     | close
 *   closed        | idle    | not_owner | not_owner     | not_owner| not_owner
 *   idle          | busy    | calling   | out_of_order  | no_reply | closed
 *   calling       | busy    | busy      | ready or idle | busy     | closed
 *   reply_ready   | busy    | busy      | out_of_order  | idle     | closed
 *
 * write and read need exactly one record, else size. An fd other than the
 * owner's is not_owner. close drops a pending reply.
 */
#include <chrysopolis/root_control.h>

#include <stddef.h>
#include <stdint.h>

static constexpr size_t runtime_ctl_words = 4;

typedef enum : uint8_t {
  /* Zero, so a BEAM restart's zeroed .bss is closed. */
  runtime_ctl_closed = 0,
  runtime_ctl_idle,
  runtime_ctl_calling,
  runtime_ctl_reply_ready,
} runtime_ctl_phase;

typedef struct {
  runtime_ctl_phase phase;
  int fd;
  chryso_ctl_reply reply;
} runtime_ctl_owner;

typedef enum : uint8_t {
  runtime_ctl_ok,
  runtime_ctl_busy,
  runtime_ctl_size,
  /* Fails the generated CtlCommand checker. */
  runtime_ctl_malformed,
  runtime_ctl_no_reply,
  /* Root's answer is not a CtlResult frame. */
  runtime_ctl_unavailable,
  runtime_ctl_not_owner,
  /* Reply with no call in flight: an adapter bug. */
  runtime_ctl_out_of_order,
} runtime_ctl_status;

typedef struct {
  uint16_t opcode;
  uint64_t args[runtime_ctl_words];
} runtime_ctl_call;

[[__nodiscard__]] runtime_ctl_status runtime_ctl_open(runtime_ctl_owner *owner,
                                                      int fd);

/* buf may be unaligned, and null only when count is 0. On ok, *call is the
 * PPC to make and the caller must deliver runtime_ctl_reply. */
[[__nodiscard__]] runtime_ctl_status runtime_ctl_write(runtime_ctl_owner *owner,
                                                       int fd, const void *buf,
                                                       size_t count,
                                                       runtime_ctl_call *call);

/* label, count and values are Root's reply frame. */
[[__nodiscard__]] runtime_ctl_status
runtime_ctl_reply(runtime_ctl_owner *owner, int fd, uint64_t label,
                  uint64_t count,
                  const uint64_t values[static runtime_ctl_words]);

/* buf may be unaligned. */
[[__nodiscard__]] runtime_ctl_status
runtime_ctl_read(runtime_ctl_owner *owner, int fd, void *buf, size_t count);

[[__nodiscard__]] runtime_ctl_status runtime_ctl_close(runtime_ctl_owner *owner,
                                                       int fd);

typedef enum : uint8_t {
  runtime_stat_copy,
  runtime_stat_eof,
  runtime_stat_short,
} runtime_stat_step;

/* One snapshot per descriptor, then EOF. */
[[__nodiscard__]] runtime_stat_step
runtime_stat_read_decide(size_t count, uint64_t position, size_t page_size);

#endif
