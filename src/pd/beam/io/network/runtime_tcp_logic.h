#ifndef CHRYSOPOLIS_RUNTIME_TCP_LOGIC_H
#define CHRYSOPOLIS_RUNTIME_TCP_LOGIC_H 1

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_tcp_state.h"

/*
 * Buffer arithmetic and checked narrowing for the vendored LionsOS socket
 * backend (tcp.c). Header-only and pure: tcp.c keeps its lwIP calls, locking
 * and ownership, and calls these helpers at the points where it previously
 * computed the same values inline. Names follow tcp.c's upstream spelling so
 * its call sites stay easy to compare with LionsOS lib/sock/tcp.c.
 *
 * The socket lifecycle lives next door in runtime_tcp_state.h, which this
 * header includes so tcp.c reaches both through one include.
 *
 * Several lwIP entry points take a u16_t where the socket layer holds a
 * size_t: a length passed straight through would be truncated modulo 65536.
 * The chunk helpers below cut a size_t down to one step lwIP can take, so the
 * caller loops instead of losing the remainder.
 */

/* Receive ring. head is the index of the oldest byte and len the number of
 * buffered bytes; every helper requires head < capacity and len <= capacity. */

/* Index where the next received byte is stored. */
[[__nodiscard__]] static inline size_t tcp_rx_tail(size_t head, size_t len,
                                                   size_t capacity) {
  return (head + len) % capacity;
}

/*
 * Contiguous free bytes starting at the tail. When the data does not wrap the
 * tail is past it and the span runs to the end of the buffer; when it wraps,
 * len exceeds the tail and the span runs up to head. Both cases reduce to
 * capacity minus the larger of len and the tail.
 */
[[__nodiscard__]] static inline size_t tcp_rx_free_span(size_t head, size_t len,
                                                        size_t capacity) {
  const size_t tail = tcp_rx_tail(head, len, capacity);
  return capacity - (len > tail ? len : tail);
}

/* Contiguous buffered bytes starting at head. */
[[__nodiscard__]] static inline size_t tcp_rx_read_span(size_t head, size_t len,
                                                        size_t capacity) {
  const size_t to_end = capacity - head;
  return len < to_end ? len : to_end;
}

/* head after consuming count bytes, count <= len. */
[[__nodiscard__]] static inline size_t tcp_rx_advance(size_t head, size_t count,
                                                      size_t capacity) {
  return (head + count) % capacity;
}

/*
 * True when incoming more bytes fit alongside the len already buffered.
 * Computed in size_t so a 2 MB capacity cannot overflow the int the free space
 * used to be narrowed to, and written as a subtraction on the capacity side so
 * the sum of len and incoming cannot wrap.
 */
[[__nodiscard__]] static inline bool
tcp_rx_can_accept(size_t len, size_t incoming, size_t capacity) {
  return len <= capacity && incoming <= capacity - len;
}

/*
 * One step of a receive-window update. tcp_recved takes a u16_t, so a larger
 * acknowledged length has to be reported over several calls; passing it
 * directly would truncate and leave the window closed.
 */
[[__nodiscard__]] static inline uint16_t tcp_recved_chunk(size_t remaining) {
  return remaining > UINT16_MAX ? UINT16_MAX : (uint16_t)remaining;
}

/*
 * How many bytes one tcp_write may take: the smaller of what the caller asked
 * for and what the send buffer holds. tcp_write takes a u16_t and tcp_sndbuf
 * already reports one, so the result needs no further check.
 */
[[__nodiscard__]] static inline uint16_t tcp_write_chunk(size_t want,
                                                         uint16_t sndbuf) {
  return want < (size_t)sndbuf ? (uint16_t)want : sndbuf;
}

/*
 * A listen backlog clamped to what tcp_listen_with_backlog accepts. It takes a
 * u8_t, so an unclamped 256 would arrive as 0 and refuse every connection. A
 * non-positive backlog becomes 1, matching the usual POSIX treatment.
 */
[[__nodiscard__]] static inline uint8_t tcp_listen_backlog(int backlog) {
  if (backlog < 1) {
    return 1;
  }
  /* UINT8_MAX is unsigned on this target, so the comparison is written on
   * the signed side to keep the operands' signedness matched. */
  return backlog > (int)UINT8_MAX ? UINT8_MAX : (uint8_t)backlog;
}

/*
 * Accept backlog: a ring of pending socket indices that keeps one slot empty
 * to tell full from empty, so it holds capacity - 1 entries. head is where
 * the next entry is written and tail the oldest entry. capacity must be
 * positive and head and tail must lie in [0, capacity).
 */

[[__nodiscard__]] static inline bool tcp_backlog_empty(int head, int tail) {
  return head == tail;
}

/* Append value, or return false without writing anything when full. */
[[__nodiscard__]] static inline bool
tcp_backlog_push(int slots[], int capacity, int *head, int tail, int value) {
  const int next = (*head + 1) % capacity;
  if (next == tail) {
    return false;
  }
  slots[*head] = value;
  *head = next;
  return true;
}

/* Remove the oldest entry into *value, or return false without writing
 * anything when empty. */
[[__nodiscard__]] static inline bool tcp_backlog_pop(const int slots[],
                                                     int capacity, int head,
                                                     int *tail, int *value) {
  if (tcp_backlog_empty(head, *tail)) {
    return false;
  }
  *value = slots[*tail];
  *tail = (*tail + 1) % capacity;
  return true;
}

/* True when a (non-positive) lwIP err_t indexes a translation table of
 * table_len entries indexed by -err. */
[[__nodiscard__]] static inline bool tcp_lwip_err_in_table(int err,
                                                           size_t table_len) {
  return err <= 0 && (size_t)(-(long)err) < table_len;
}

#endif
