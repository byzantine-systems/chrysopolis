/* runtime_tcp_logic.h: receive ring spans, accept backlog, lwIP error table
 * bounds and close transitions. */
#include "check.h"

#include "runtime_tcp_logic.h"

#include <limits.h>
#include <string.h>

/* Contiguous free bytes after the tail, counted one slot at a time. */
static size_t model_free_span(size_t head, size_t len, size_t capacity) {
  size_t span = 0;
  for (size_t i = (head + len) % capacity; span < capacity - len; i++) {
    if (i == capacity) {
      break;
    }
    span++;
  }
  return span;
}

/* Every head and length for small capacities. */
static void test_ring_spans_exhaustive(void) {
  for (size_t capacity = 1; capacity <= 9; capacity++) {
    for (size_t head = 0; head < capacity; head++) {
      for (size_t len = 0; len <= capacity; len++) {
        const size_t free_span = tcp_rx_free_span(head, len, capacity);
        const size_t read_span = tcp_rx_read_span(head, len, capacity);
        CHECK_EQ_U64(free_span, model_free_span(head, len, capacity));
        CHECK(free_span <= capacity - len);
        CHECK(tcp_rx_tail(head, len, capacity) < capacity);
        CHECK(read_span <= len && head + read_span <= capacity);
        /* Nothing buffered or nothing free leaves nothing to do. */
        CHECK(len != 0 || read_span == 0);
        CHECK(len != capacity || free_span == 0);
        /* Any non-full ring has room at the tail, and any non-empty ring has
         * data at the head, so the copy loops always make progress. */
        CHECK(len == capacity || free_span > 0);
        CHECK(len == 0 || read_span > 0);
      }
    }
  }
}

enum : size_t { ring_capacity = 13, model_capacity = 4096 };

typedef struct {
  unsigned char buf[ring_capacity];
  size_t head;
  size_t len;
} ring;

/* Mirror tcp.c's receive callback: copy in contiguous spans. */
static void ring_write(ring *r, const unsigned char *data, size_t count) {
  size_t copied = 0;
  while (copied < count) {
    const size_t tail = tcp_rx_tail(r->head, r->len, ring_capacity);
    size_t span = tcp_rx_free_span(r->head, r->len, ring_capacity);
    span = span < count - copied ? span : count - copied;
    memcpy(r->buf + tail, data + copied, span);
    r->len += span;
    copied += span;
  }
}

/* Mirror tcp.c's recv: copy out contiguous spans. */
static size_t ring_read(ring *r, unsigned char *out, size_t count) {
  size_t copied = 0;
  while (copied < count) {
    size_t span = tcp_rx_read_span(r->head, r->len, ring_capacity);
    span = span < count - copied ? span : count - copied;
    if (span == 0) {
      break;
    }
    memcpy(out + copied, r->buf + r->head, span);
    r->head = tcp_rx_advance(r->head, span, ring_capacity);
    r->len -= span;
    copied += span;
  }
  return copied;
}

/* Bytes come out in the order they went in across many wraparounds. */
static void test_ring_fifo_cycles(void) {
  ring r = {};
  unsigned char expected[model_capacity] = {};
  size_t produced = 0;
  size_t consumed = 0;
  unsigned char next = 0;

  for (size_t step = 0; step < 400; step++) {
    const size_t want_write = (step * 7) % 6;
    const size_t room = ring_capacity - r.len;
    const size_t write = want_write < room ? want_write : room;
    unsigned char chunk[ring_capacity] = {};
    for (size_t i = 0; i < write; i++) {
      chunk[i] = next;
      expected[(produced + i) % model_capacity] = next;
      next++;
    }
    ring_write(&r, chunk, write);
    produced += write;

    unsigned char out[ring_capacity] = {};
    const size_t got = ring_read(&r, out, (step * 5) % 7);
    for (size_t i = 0; i < got; i++) {
      CHECK(out[i] == expected[(consumed + i) % model_capacity]);
    }
    consumed += got;
    CHECK_EQ_U64(r.len, produced - consumed);
    CHECK(r.head < ring_capacity);
  }
  CHECK(produced > 10 * ring_capacity);
}

enum : int { backlog = 4 };

static void test_backlog_fill_and_drain(void) {
  int slots[backlog] = {};
  int head = 0;
  int tail = 0;
  int value = -1;

  CHECK(tcp_backlog_empty(head, tail));
  CHECK(!tcp_backlog_pop(slots, backlog, head, &tail, &value));
  CHECK(value == -1 && tail == 0);

  /* One slot stays empty, so the ring holds capacity - 1 entries. */
  for (int i = 0; i < backlog - 1; i++) {
    CHECK(tcp_backlog_push(slots, backlog, &head, tail, 100 + i));
  }
  const int full_head = head;
  const int before[backlog] = {slots[0], slots[1], slots[2], slots[3]};
  CHECK(!tcp_backlog_push(slots, backlog, &head, tail, 999));
  CHECK(head == full_head);
  CHECK(memcmp(slots, before, sizeof(slots)) == 0);

  for (int i = 0; i < backlog - 1; i++) {
    CHECK(tcp_backlog_pop(slots, backlog, head, &tail, &value));
    CHECK(value == 100 + i);
  }
  CHECK(tcp_backlog_empty(head, tail));
}

/* Interleaved pushes and pops wrap both indices many times and keep order. */
static void test_backlog_wraparound(void) {
  int slots[backlog] = {};
  int head = 0;
  int tail = 0;
  int pushed = 0;
  int popped = 0;
  for (int round = 0; round < 50; round++) {
    const int pushes = round % 3 + 1;
    for (int i = 0; i < pushes; i++) {
      if (tcp_backlog_push(slots, backlog, &head, tail, pushed)) {
        pushed++;
      }
    }
    const int pops = (round * 2) % 3 + 1;
    for (int i = 0; i < pops; i++) {
      int value = -1;
      if (tcp_backlog_pop(slots, backlog, head, &tail, &value)) {
        CHECK(value == popped);
        popped++;
      }
    }
    CHECK(head >= 0 && head < backlog && tail >= 0 && tail < backlog);
    CHECK(pushed - popped >= 0 && pushed - popped <= backlog - 1);
  }
  CHECK(popped > 3 * backlog);
}

static void test_lwip_err_bounds(void) {
  static constexpr size_t table_len = 20;
  CHECK(tcp_lwip_err_in_table(0, table_len));
  CHECK(tcp_lwip_err_in_table(-1, table_len));
  CHECK(tcp_lwip_err_in_table(-19, table_len));
  CHECK(!tcp_lwip_err_in_table(-20, table_len));
  CHECK(!tcp_lwip_err_in_table(-128, table_len));
  CHECK(!tcp_lwip_err_in_table(1, table_len));
  CHECK(!tcp_lwip_err_in_table(127, table_len));
  CHECK(!tcp_lwip_err_in_table(INT_MIN, table_len));
  CHECK(!tcp_lwip_err_in_table(INT_MAX, table_len));
  CHECK(!tcp_lwip_err_in_table(0, 0));
}

static void test_close_actions(void) {
  CHECK(tcp_close_action(socket_state_listening) == tcp_close_begin);
  CHECK(tcp_close_action(socket_state_connected) == tcp_close_begin);
  CHECK(tcp_close_action(socket_state_connecting) ==
        tcp_close_abort_connecting);
  CHECK(tcp_close_action(socket_state_closed_by_peer) ==
        tcp_close_release_peer_closed);
  CHECK(tcp_close_action(socket_state_allocated) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_bound) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_error) == tcp_close_release_idle);
  CHECK(tcp_close_action(socket_state_unallocated) == tcp_close_invalid);
  CHECK(tcp_close_action(socket_state_closing) == tcp_close_invalid);
  CHECK(tcp_close_action((socket_state_t)42) == tcp_close_invalid);

  /* The upstream ordinal values are what TCP_DEBUG traces print. */
  CHECK(socket_state_unallocated == 0);
  CHECK(socket_state_closed_by_peer == 6);
  CHECK(socket_state_listening == 8);
}

int main(void) {
  test_ring_spans_exhaustive();
  test_ring_fifo_cycles();
  test_backlog_fill_and_drain();
  test_backlog_wraparound();
  test_lwip_err_bounds();
  test_close_actions();
  return check_finish("tcp_logic");
}
