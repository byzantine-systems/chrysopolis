#ifndef CHRYSOPOLIS_RUNTIME_TCP_LOGIC_H
#define CHRYSOPOLIS_RUNTIME_TCP_LOGIC_H 1

#include <stdbool.h>
#include <stddef.h>

/*
 * Buffer arithmetic and state decisions for the vendored LionsOS socket
 * backend (tcp.c). Header-only and pure: tcp.c keeps its lwIP calls, locking
 * and ownership, and calls these helpers at the points where it previously
 * computed the same values inline. Names follow tcp.c's upstream spelling so
 * its call sites stay easy to compare with LionsOS lib/sock/tcp.c.
 */

typedef enum {
  socket_state_unallocated,
  socket_state_allocated,
  socket_state_bound,
  socket_state_connecting,
  socket_state_connected,
  socket_state_closing,
  socket_state_closed_by_peer,
  socket_state_error,
  socket_state_listening,
} socket_state_t;

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

/* What close(2) must do for the last reference to a socket. */
typedef enum {
  /* Listening or connected: start lwIP's close, the PCB is released later. */
  tcp_close_begin,
  /* Connecting: abort the PCB and release the socket now. */
  tcp_close_abort_connecting,
  /* Peer already closed: detach callbacks, close or abort, release now. */
  tcp_close_release_peer_closed,
  /* No live connection (allocated, bound or failed): release now. */
  tcp_close_release_idle,
  /* Already unallocated or closing: a caller bug. */
  tcp_close_invalid,
} tcp_close_action_t;

[[__nodiscard__]] static inline tcp_close_action_t
tcp_close_action(socket_state_t state) {
  switch (state) {
  case socket_state_listening:
  case socket_state_connected:
    return tcp_close_begin;
  case socket_state_connecting:
    return tcp_close_abort_connecting;
  case socket_state_closed_by_peer:
    return tcp_close_release_peer_closed;
  case socket_state_allocated:
  case socket_state_bound:
  case socket_state_error:
    return tcp_close_release_idle;
  case socket_state_unallocated:
  case socket_state_closing:
    break;
  }
  return tcp_close_invalid;
}

#endif
