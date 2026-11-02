#ifndef CHRYSOPOLIS_RUNTIME_TRACE_RING_H
#define CHRYSOPOLIS_RUNTIME_TRACE_RING_H 1

#include <stddef.h>
#include <stdint.h>

/*
 * Bounded wake-path trace buffer. Pure: the caller owns the records and the
 * ring state, and reads the counter and cothread id.
 *
 * A full ring stops and counts what it drops, so a captured window is never
 * overwritten.
 *
 *   state  event          next   effect
 *   off    arm            armed  length = dropped = 0
 *   off    push           off    ignored
 *   armed  push, room     armed  record stored
 *   armed  push, no room  full   dropped = 1
 *   full   push           full   dropped += 1, saturating
 *   any    disarm         off    records kept for reading
 *   any    arm            armed  length = dropped = 0
 *
 * O(1). Unchecked errors: null arguments, records shorter than capacity.
 */

typedef enum : uint8_t {
  runtime_trace_futex_wait,
  runtime_trace_futex_timeout,
  runtime_trace_futex_wake,
  runtime_trace_park,
  runtime_trace_resume,
  runtime_trace_epoll_enter,
  runtime_trace_epoll_exit,
  runtime_trace_notified,
  runtime_trace_drain_rearm,
  runtime_trace_event_count,
} runtime_trace_event;

typedef enum : uint8_t {
  runtime_trace_off,
  runtime_trace_armed,
  runtime_trace_full,
} runtime_trace_state;

/* One record; its layout is the /dev/chrysotrace wire format, little endian. */
typedef struct {
  uint64_t ticks;
  int64_t a;
  int64_t b;
  uint8_t event;
  int8_t cothread;
  uint8_t reserved[6];
} runtime_trace_record;

static_assert(sizeof(runtime_trace_record) == 32);

typedef struct {
  runtime_trace_state state;
  uint32_t length;
  uint32_t dropped;
} runtime_trace_ring;

void runtime_trace_ring_arm(runtime_trace_ring *ring);

void runtime_trace_ring_disarm(runtime_trace_ring *ring);

/* Store `record` if armed and there is room; see the table above. */
void runtime_trace_ring_push(runtime_trace_ring *ring,
                             runtime_trace_record records[], size_t capacity,
                             const runtime_trace_record *record);

#endif
