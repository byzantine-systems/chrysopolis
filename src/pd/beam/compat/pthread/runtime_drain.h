#ifndef CHRYSOPOLIS_RUNTIME_DRAIN_H
#define CHRYSOPOLIS_RUNTIME_DRAIN_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * Root-thread drain decision. Pure.
 *
 * A yielding cothread queues behind the root; if the root went back to the
 * event loop, it would sit unrun until an unrelated notification. The root
 * yields while anything is ready, at most `limit` rounds, then arms an
 * immediate timeout so other notifications still get served.
 *
 *   runnable  rounds < limit  step
 *   false     any             idle   return to the event loop
 *   true      true            run    yield one round
 *   true      false           rearm  arm an immediate timeout, then return
 */

typedef enum : uint8_t {
  runtime_drain_idle,
  runtime_drain_run,
  runtime_drain_rearm,
} runtime_drain_step;

[[__nodiscard__]] static inline runtime_drain_step
runtime_drain_next(bool runnable, uint32_t rounds, uint32_t limit) {
  if (!runnable) {
    return runtime_drain_idle;
  }
  return rounds < limit ? runtime_drain_run : runtime_drain_rearm;
}

#endif
