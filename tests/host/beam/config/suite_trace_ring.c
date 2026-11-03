/* runtime_trace_ring.h: every transition-table row, then random arm, disarm
 * and push sequences against a counter model. */
#include "check.h"

#include "runtime_trace_ring.h"

#include <stdint.h>

enum : size_t { capacity = 4 };

static runtime_trace_record rec(int64_t a) {
  return (runtime_trace_record){.ticks = (uint64_t)a, .a = a, .event = 1};
}

static void test_rows(void) {
  runtime_trace_record records[capacity] = {};
  runtime_trace_ring ring = {};
  const runtime_trace_record r = rec(7);

  /* off + push: ignored. */
  runtime_trace_ring_push(&ring, records, capacity, &r);
  CHECK(ring.state == runtime_trace_off);
  CHECK_EQ_U64(ring.length, 0);
  /* arm, then fill. */
  runtime_trace_ring_arm(&ring);
  for (int64_t i = 0; i < (int64_t)capacity; i++) {
    const runtime_trace_record x = rec(i);
    runtime_trace_ring_push(&ring, records, capacity, &x);
  }
  CHECK(ring.state == runtime_trace_armed);
  CHECK_EQ_U64(ring.length, capacity);
  CHECK(records[3].a == 3);
  /* armed, no room: full with one drop; the window is kept. */
  runtime_trace_ring_push(&ring, records, capacity, &r);
  CHECK(ring.state == runtime_trace_full);
  CHECK_EQ_U64(ring.dropped, 1);
  CHECK(records[3].a == 3);
  runtime_trace_ring_push(&ring, records, capacity, &r);
  CHECK_EQ_U64(ring.dropped, 2);
  /* full saturates. */
  ring.dropped = UINT32_MAX;
  runtime_trace_ring_push(&ring, records, capacity, &r);
  CHECK_EQ_U64(ring.dropped, UINT32_MAX);
  /* disarm keeps records. */
  runtime_trace_ring_disarm(&ring);
  CHECK(ring.state == runtime_trace_off);
  CHECK_EQ_U64(ring.length, capacity);
  /* re-arm clears. */
  runtime_trace_ring_arm(&ring);
  CHECK(ring.state == runtime_trace_armed);
  CHECK_EQ_U64(ring.length, 0);
  CHECK_EQ_U64(ring.dropped, 0);
}

/* xorshift64, fixed seed so a failure replays. */
static uint64_t next(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return *s;
}

static void test_model(void) {
  uint64_t seed = 0x7ace'0001;
  for (int run = 0; run < 500; run++) {
    runtime_trace_record records[capacity] = {};
    runtime_trace_ring ring = {};
    bool armed = false;
    uint32_t stored = 0;
    uint32_t dropped = 0;
    for (int step = 0; step < 40; step++) {
      const uint64_t pick = next(&seed) % 10;
      if (pick == 0) {
        runtime_trace_ring_arm(&ring);
        armed = true;
        stored = dropped = 0;
      } else if (pick == 1) {
        runtime_trace_ring_disarm(&ring);
        armed = false;
      } else {
        const runtime_trace_record x = rec(step);
        runtime_trace_ring_push(&ring, records, capacity, &x);
        if (armed && stored < capacity) {
          CHECK(records[stored].a == step);
          stored++;
        } else if (armed) {
          dropped++;
        }
      }
      CHECK_EQ_U64(ring.length, stored);
      CHECK_EQ_U64(ring.dropped, dropped);
      CHECK((ring.state != runtime_trace_off) == armed);
      CHECK((ring.state == runtime_trace_full) == (armed && dropped > 0));
    }
  }
}

int main(void) {
  test_rows();
  test_model();
  return check_finish("trace_ring");
}
