/* runtime_wake_pipe.h: the transition table, then random operation sequences
 * against a counter model. */
#include "check.h"
#include "prop.h"

#include "runtime_wake_pipe.h"

#include <stdint.h>
#include <stdio.h>

enum : size_t { slots = 3, max_ops = 64, cases = 2000 };

static void test_table_rows(void) {
  runtime_pipe_slot t[slots] = {};
  size_t n = 0;

  CHECK(runtime_pipe_open(t, slots, 10, 11) == runtime_pipe_ok);
  CHECK(!runtime_pipe_ready(t, slots, 10));
  /* empty + read: would_block. */
  CHECK(runtime_pipe_read(t, slots, 10, 8, &n) == runtime_pipe_would_block);
  CHECK_EQ_U64(n, 0);
  /* empty + write: readable. */
  CHECK(runtime_pipe_write(t, slots, 11, 1, &n) == runtime_pipe_ok);
  CHECK_EQ_U64(n, 1);
  CHECK(runtime_pipe_ready(t, slots, 10));
  CHECK(!runtime_pipe_ready(t, slots, 11));
  /* readable + read some, then all. */
  CHECK(runtime_pipe_write(t, slots, 11, 2, &n) == runtime_pipe_ok);
  CHECK(runtime_pipe_read(t, slots, 10, 2, &n) == runtime_pipe_ok);
  CHECK_EQ_U64(n, 2);
  CHECK(runtime_pipe_ready(t, slots, 10));
  CHECK(runtime_pipe_read(t, slots, 10, 8, &n) == runtime_pipe_ok);
  CHECK_EQ_U64(n, 1);
  CHECK(!runtime_pipe_ready(t, slots, 10));
  /* Full: the excess is refused, never wrapped. */
  CHECK(runtime_pipe_write(t, slots, 11, runtime_pipe_capacity + 5, &n) ==
        runtime_pipe_ok);
  CHECK_EQ_U64(n, runtime_pipe_capacity);
  CHECK(runtime_pipe_write(t, slots, 11, 1, &n) == runtime_pipe_would_block);
  CHECK(runtime_pipe_read(t, slots, 10, SIZE_MAX, &n) == runtime_pipe_ok);
  CHECK_EQ_U64(n, runtime_pipe_capacity);
  /* Ends are not interchangeable. */
  CHECK(runtime_pipe_write(t, slots, 10, 1, &n) == runtime_pipe_unknown_fd);
  CHECK(runtime_pipe_read(t, slots, 11, 1, &n) == runtime_pipe_unknown_fd);
  /* Closed writer: empty reads EOF and reads as ready. */
  CHECK(runtime_pipe_close(t, slots, 11) == runtime_pipe_ok);
  CHECK(runtime_pipe_read(t, slots, 10, 1, &n) == runtime_pipe_eof);
  CHECK(runtime_pipe_ready(t, slots, 10));
  /* Last end closed: the slot is free and its fds are unknown. */
  CHECK(runtime_pipe_close(t, slots, 10) == runtime_pipe_ok);
  CHECK(t[0].state == runtime_pipe_free);
  CHECK(runtime_pipe_close(t, slots, 10) == runtime_pipe_unknown_fd);
  /* Closed reader: writes are broken. */
  CHECK(runtime_pipe_open(t, slots, 20, 21) == runtime_pipe_ok);
  CHECK(runtime_pipe_close(t, slots, 20) == runtime_pipe_ok);
  CHECK(runtime_pipe_write(t, slots, 21, 1, &n) == runtime_pipe_broken);
  /* A full table refuses another pipe. */
  CHECK(runtime_pipe_open(t, slots, 30, 31) == runtime_pipe_ok);
  CHECK(runtime_pipe_open(t, slots, 40, 41) == runtime_pipe_ok);
  CHECK(runtime_pipe_open(t, slots, 50, 51) == runtime_pipe_no_slot);
}

typedef enum : uint8_t { op_open, op_write, op_read, op_close } op_kind;

typedef struct {
  op_kind kind;
  uint8_t pipe; /* model pipe index, its fds are 2*pipe and 2*pipe+1 */
  bool writer;
  uint16_t bytes;
} op;

/* The model: per pipe, whether each end is open and how many bytes wait. */
typedef struct {
  bool read_open[slots + 1];
  bool write_open[slots + 1];
  uint32_t pending[slots + 1];
} model;

static size_t live(const model *m) {
  size_t k = 0;
  for (size_t p = 0; p <= slots; p++) {
    k += m->read_open[p] || m->write_open[p];
  }
  return k;
}

/* Replays ops on both sides; false at the first disagreement. */
static bool agree(const void *items, size_t count, void *ctx) {
  (void)ctx;
  const op *ops = items;
  runtime_pipe_slot t[slots] = {};
  model m = {};
  for (size_t i = 0; i < count; i++) {
    const op o = ops[i];
    const int rfd = 2 * o.pipe;
    const int wfd = rfd + 1;
    const bool open = m.read_open[o.pipe] || m.write_open[o.pipe];
    size_t n = 0;
    runtime_pipe_status got = runtime_pipe_ok;
    runtime_pipe_status want = runtime_pipe_ok;
    size_t want_n = 0;
    switch (o.kind) {
    case op_open:
      if (open) {
        continue; /* fds are still in use; not a legal call */
      }
      got = runtime_pipe_open(t, slots, rfd, wfd);
      want = live(&m) < slots ? runtime_pipe_ok : runtime_pipe_no_slot;
      if (want == runtime_pipe_ok) {
        m.read_open[o.pipe] = m.write_open[o.pipe] = true;
        m.pending[o.pipe] = 0;
      }
      break;
    case op_write:
      got = runtime_pipe_write(t, slots, wfd, o.bytes, &n);
      if (!m.write_open[o.pipe]) {
        want = runtime_pipe_unknown_fd;
      } else if (!m.read_open[o.pipe]) {
        want = runtime_pipe_broken;
      } else {
        const uint32_t room = runtime_pipe_capacity - m.pending[o.pipe];
        want = o.bytes != 0 && room == 0 ? runtime_pipe_would_block
                                         : runtime_pipe_ok;
        want_n = o.bytes < room ? o.bytes : room;
        m.pending[o.pipe] += (uint32_t)want_n;
      }
      break;
    case op_read:
      got = runtime_pipe_read(t, slots, rfd, o.bytes, &n);
      if (!m.read_open[o.pipe]) {
        want = runtime_pipe_unknown_fd;
      } else if (m.pending[o.pipe] == 0) {
        want =
            m.write_open[o.pipe] ? runtime_pipe_would_block : runtime_pipe_eof;
      } else {
        want_n = o.bytes < m.pending[o.pipe] ? o.bytes : m.pending[o.pipe];
        m.pending[o.pipe] -= (uint32_t)want_n;
      }
      break;
    case op_close: {
      const bool end_open =
          o.writer ? m.write_open[o.pipe] : m.read_open[o.pipe];
      got = runtime_pipe_close(t, slots, o.writer ? wfd : rfd);
      want = end_open ? runtime_pipe_ok : runtime_pipe_unknown_fd;
      if (o.writer) {
        m.write_open[o.pipe] = false;
      } else {
        m.read_open[o.pipe] = false;
      }
      break;
    }
    }
    if (got != want || n != want_n) {
      return false;
    }
    const bool ready = m.read_open[o.pipe] &&
                       (m.pending[o.pipe] != 0 || !m.write_open[o.pipe]);
    if (runtime_pipe_ready(t, slots, rfd) != ready) {
      return false;
    }
  }
  return true;
}

static op gen_op(prop_rng *rng) {
  /* Byte counts near zero, small, and near capacity. */
  static const uint16_t sizes[] = {0, 1, 2, 7, 4095, 4096, 4097};
  return (op){
      .kind = (op_kind)prop_below(rng, 4),
      .pipe = (uint8_t)prop_below(rng, slots + 1),
      .writer = prop_chance(rng, 50),
      .bytes = sizes[prop_below(rng, sizeof sizes / sizeof sizes[0])],
  };
}

static void test_model(void) {
  prop_rng rng = prop_seed(0x5eed'91be);
  op ops[max_ops] = {};
  op scratch[max_ops] = {};
  for (size_t c = 0; c < cases; c++) {
    const size_t count = 1 + prop_below(&rng, max_ops);
    for (size_t i = 0; i < count; i++) {
      ops[i] = gen_op(&rng);
    }
    if (!agree(ops, count, nullptr)) {
      const size_t kept =
          prop_shrink(ops, count, sizeof ops[0], scratch, agree, nullptr);
      printf("wake_pipe model failed, case %zu; shrunk to %zu ops:\n", c, kept);
      for (size_t i = 0; i < kept; i++) {
        printf("  kind=%d pipe=%u writer=%d bytes=%u\n", ops[i].kind,
               ops[i].pipe, ops[i].writer, ops[i].bytes);
      }
      CHECK(false);
      return;
    }
  }
}

int main(void) {
  test_table_rows();
  test_model();
  return check_finish("wake_pipe");
}
