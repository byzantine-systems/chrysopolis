/* runtime_tcp_logic.h: receive ring spans, checked narrowing for the lwIP
 * calls that take a u16_t or a u8_t, accept backlog, and error table bounds.
 * The socket lifecycle it now includes is covered by suite_tcp_state.c. */
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

/* The socket buffer tcp.c actually uses, so the boundary cases below are the
 * ones the running system hits. */
enum : size_t { socket_buf_size = 0x200000 };

static void test_rx_can_accept(void) {
  /* An empty ring takes anything up to its capacity, and nothing past it. */
  CHECK(tcp_rx_can_accept(0, 0, socket_buf_size));
  CHECK(tcp_rx_can_accept(0, socket_buf_size, socket_buf_size));
  CHECK(!tcp_rx_can_accept(0, socket_buf_size + 1, socket_buf_size));

  /* A full ring takes nothing but a zero-length segment. */
  CHECK(tcp_rx_can_accept(socket_buf_size, 0, socket_buf_size));
  CHECK(!tcp_rx_can_accept(socket_buf_size, 1, socket_buf_size));

  /* The exact fit, and one byte past it. */
  CHECK(tcp_rx_can_accept(socket_buf_size - 1, 1, socket_buf_size));
  CHECK(!tcp_rx_can_accept(socket_buf_size - 1, 2, socket_buf_size));

  /* The free space used to be narrowed to an int. A capacity larger than
   * INT_MAX must still answer correctly. */
  static constexpr size_t huge = (size_t)INT_MAX + 4096;
  CHECK(tcp_rx_can_accept(0, huge, huge));
  CHECK(!tcp_rx_can_accept(1, huge, huge));

  /* Neither side may wrap: an incoming length near SIZE_MAX is refused, not
   * accepted through an overflowed sum. */
  CHECK(!tcp_rx_can_accept(1, SIZE_MAX, socket_buf_size));
  CHECK(!tcp_rx_can_accept(SIZE_MAX, 1, socket_buf_size));
  CHECK(!tcp_rx_can_accept(SIZE_MAX, SIZE_MAX, socket_buf_size));

  /* Agreement with the ring arithmetic: whatever fits is what the free
   * span can absorb over successive copies. */
  for (size_t capacity = 1; capacity <= 9; capacity++) {
    for (size_t len = 0; len <= capacity; len++) {
      for (size_t incoming = 0; incoming <= capacity + 1; incoming++) {
        CHECK(tcp_rx_can_accept(len, incoming, capacity) ==
              (incoming <= capacity - len));
      }
    }
  }
}

/* A window update of any size is reported in full across repeated chunks. */
static void test_recved_chunk(void) {
  CHECK_EQ_U64(tcp_recved_chunk(0), 0);
  CHECK_EQ_U64(tcp_recved_chunk(1), 1);
  CHECK_EQ_U64(tcp_recved_chunk(UINT16_MAX - 1), UINT16_MAX - 1);
  CHECK_EQ_U64(tcp_recved_chunk(UINT16_MAX), UINT16_MAX);
  CHECK_EQ_U64(tcp_recved_chunk((size_t)UINT16_MAX + 1), UINT16_MAX);
  CHECK_EQ_U64(tcp_recved_chunk(SIZE_MAX), UINT16_MAX);

  /* The lengths that spelled trouble: a plain (u16_t)len truncates 65536 to
   * 0 and leaves the receive window shut. */
  static const size_t lengths[] = {
      0,
      1,
      1024,
      UINT16_MAX - 1,
      UINT16_MAX,
      (size_t)UINT16_MAX + 1,
      (size_t)UINT16_MAX * 2,
      131072,
      socket_buf_size,
  };
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    size_t remaining = lengths[i];
    size_t reported = 0;
    size_t steps = 0;
    while (remaining != 0) {
      const uint16_t chunk = tcp_recved_chunk(remaining);
      CHECK(chunk != 0);
      CHECK(chunk <= remaining);
      remaining -= chunk;
      reported += chunk;
      steps++;
      CHECK(steps <= lengths[i] / UINT16_MAX + 1);
    }
    CHECK_EQ_U64(reported, lengths[i]);
  }
}

static void test_write_chunk(void) {
  /* Bounded by both sides, and zero exactly when either side is zero. */
  CHECK_EQ_U64(tcp_write_chunk(0, 0), 0);
  CHECK_EQ_U64(tcp_write_chunk(0, 100), 0);
  CHECK_EQ_U64(tcp_write_chunk(100, 0), 0);
  CHECK_EQ_U64(tcp_write_chunk(10, 100), 10);
  CHECK_EQ_U64(tcp_write_chunk(100, 10), 10);
  CHECK_EQ_U64(tcp_write_chunk(100, 100), 100);
  CHECK_EQ_U64(tcp_write_chunk(UINT16_MAX, UINT16_MAX), UINT16_MAX);
  /* A caller asking for more than a u16 gets the send buffer, never a
   * truncated remainder. */
  CHECK_EQ_U64(tcp_write_chunk((size_t)UINT16_MAX + 1, UINT16_MAX), UINT16_MAX);
  CHECK_EQ_U64(tcp_write_chunk(socket_buf_size, 1460), 1460);
  CHECK_EQ_U64(tcp_write_chunk(SIZE_MAX, 1), 1);

  for (size_t want = 0; want <= 300; want++) {
    for (uint16_t sndbuf = 0; sndbuf <= 300; sndbuf++) {
      const uint16_t got = tcp_write_chunk(want, sndbuf);
      CHECK(got <= sndbuf);
      CHECK((size_t)got <= want);
      /* Whatever both sides allow is taken, never less. */
      CHECK_EQ_U64(got, want < sndbuf ? want : sndbuf);
    }
  }
}

static void test_listen_backlog(void) {
  /* tcp_listen_with_backlog takes a u8_t: 256 arrived as 0 and refused
   * every connection. */
  CHECK_EQ_U64(tcp_listen_backlog(256), UINT8_MAX);
  CHECK_EQ_U64(tcp_listen_backlog(255), 255);
  CHECK_EQ_U64(tcp_listen_backlog(254), 254);
  CHECK_EQ_U64(tcp_listen_backlog(INT_MAX), UINT8_MAX);

  /* A non-positive backlog still has to accept one connection. */
  CHECK_EQ_U64(tcp_listen_backlog(1), 1);
  CHECK_EQ_U64(tcp_listen_backlog(0), 1);
  CHECK_EQ_U64(tcp_listen_backlog(-1), 1);
  CHECK_EQ_U64(tcp_listen_backlog(INT_MIN), 1);

  /* Never zero, never above the u8 range, monotonic in between. */
  uint8_t previous = tcp_listen_backlog(INT_MIN);
  for (int backlog = -4; backlog <= 300; backlog++) {
    const uint8_t got = tcp_listen_backlog(backlog);
    CHECK(got >= 1);
    CHECK(got >= previous);
    previous = got;
  }
  CHECK_EQ_U64(previous, UINT8_MAX);
}

int main(void) {
  test_ring_spans_exhaustive();
  test_ring_fifo_cycles();
  test_rx_can_accept();
  test_recved_chunk();
  test_write_chunk();
  test_listen_backlog();
  test_backlog_fill_and_drain();
  test_backlog_wraparound();
  test_lwip_err_bounds();
  return check_finish("tcp_logic");
}
