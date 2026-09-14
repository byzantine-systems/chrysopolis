/*
 * Timer multiplexer for the single sDDF timeout slot.
 *
 * lwIP, sleep, poll, and condition waits all provide absolute deadlines. The
 * nearest deadline owns the slot; later deadlines never displace it. After a
 * notification clears the slot, each awakened waiter rechecks its predicate
 * and rearms any remaining time, so sharing does not lose longer deadlines.
 */
#include "runtime_timer.h"
#include "runtime_config.h"

#include <sddf/timer/client.h>

static uint64_t beam_timer_deadline;

void beam_timer_arm(uint64_t deadline_ns) {
  const uint64_t now = sddf_timer_time_now(timer_config.driver_id);
  if (deadline_ns <= now) {
    deadline_ns = now + 1;
  }

  /* A restarted timer driver notifies clients whose timeouts it discarded.
   * That notification clears this slot, so an elapsed deadline must not be
   * cleared here: it may already be pending delivery. */
  if (beam_timer_deadline == 0 || deadline_ns < beam_timer_deadline) {
    beam_timer_deadline = deadline_ns;
    sddf_timer_set_timeout(timer_config.driver_id, deadline_ns - now);
  }
}

void runtime_timer_notified(microkit_channel ch) {
  if (ch == timer_config.driver_id) {
    beam_timer_deadline = 0;
  }
}
