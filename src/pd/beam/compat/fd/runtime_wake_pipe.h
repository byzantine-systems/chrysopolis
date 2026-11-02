#ifndef CHRYSOPOLIS_RUNTIME_WAKE_PIPE_H
#define CHRYSOPOLIS_RUNTIME_WAKE_PIPE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Byte-counting pipes for ERTS's self-pipes. Pure: the caller owns the table
 * and the descriptors.
 *
 * erts_poll_wait leaves its wait only when the wakeup pipe reads as ready, so
 * a write must leave the read end readable until a read drains it. No data is
 * kept, only how many bytes are unread.
 *
 *   state     event            next      result
 *   free      open             empty     ok
 *   empty     write n          readable  ok (n bytes, up to capacity)
 *   readable  write n          readable  ok, or would_block when full
 *   empty     read             empty     would_block, or eof once the write
 *                                        end is closed
 *   readable  read takes all   empty     ok
 *   readable  read takes some  readable  ok
 *   any       write, no reader same      broken
 *   any       close last end   free      ok
 *
 * Every call scans the table: O(count). Unchecked errors: a null table or
 * out-parameter, a table shorter than count, and opening an fd already in
 * the table. Zero-byte reads and writes change nothing.
 */

typedef enum : uint8_t {
  runtime_pipe_free,
  runtime_pipe_empty,
  runtime_pipe_readable,
} runtime_pipe_state;

typedef enum : uint8_t {
  runtime_pipe_ok,
  runtime_pipe_would_block,
  runtime_pipe_eof,
  runtime_pipe_broken,
  runtime_pipe_no_slot,
  runtime_pipe_unknown_fd,
} runtime_pipe_status;

/* Caller storage, zero-initialised; only these functions write it. */
typedef struct {
  runtime_pipe_state state;
  bool read_open;
  bool write_open;
  int read_fd;
  int write_fd;
  uint32_t pending;
} runtime_pipe_slot;

/* Unread bytes one pipe holds, as Linux's minimum pipe capacity. */
static constexpr uint32_t runtime_pipe_capacity = 4096;

/* Claim a free slot for the two descriptors. no_slot when the table is full. */
[[__nodiscard__]] runtime_pipe_status
runtime_pipe_open(runtime_pipe_slot *table, size_t count, int read_fd,
                  int write_fd);

/* Write `bytes` through write end `fd`; *accepted gets how many fit. */
[[__nodiscard__]] runtime_pipe_status
runtime_pipe_write(runtime_pipe_slot *table, size_t count, int fd, size_t bytes,
                   size_t *accepted);

/* Read up to `bytes` through read end `fd`; *taken gets how many. */
[[__nodiscard__]] runtime_pipe_status
runtime_pipe_read(runtime_pipe_slot *table, size_t count, int fd, size_t bytes,
                  size_t *taken);

/* True when `fd` is a read end with unread bytes, or whose writer closed. */
[[__nodiscard__]] bool runtime_pipe_ready(const runtime_pipe_slot *table,
                                          size_t count, int fd);

/* Close one end; the slot is freed with its last end. */
[[__nodiscard__]] runtime_pipe_status
runtime_pipe_close(runtime_pipe_slot *table, size_t count, int fd);

#endif
