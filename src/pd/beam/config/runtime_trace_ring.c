#include "runtime_trace_ring.h"

void runtime_trace_ring_arm(runtime_trace_ring *ring) {
  *ring = (runtime_trace_ring){.state = runtime_trace_armed};
}

void runtime_trace_ring_disarm(runtime_trace_ring *ring) {
  ring->state = runtime_trace_off;
}

void runtime_trace_ring_push(runtime_trace_ring *ring,
                             runtime_trace_record records[], size_t capacity,
                             const runtime_trace_record *record) {
  switch (ring->state) {
  case runtime_trace_off:
    return;
  case runtime_trace_armed:
    if (ring->length < capacity) {
      records[ring->length] = *record;
      ring->length++;
      return;
    }
    ring->state = runtime_trace_full;
    ring->dropped = 1;
    return;
  case runtime_trace_full:
    if (ring->dropped != UINT32_MAX) {
      ring->dropped++;
    }
    return;
  }
}
