#ifndef CHRYSOPOLIS_RUNTIME_TIMER_SLOT_H
#define CHRYSOPOLIS_RUNTIME_TIMER_SLOT_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * Arming rule for the PD's single sDDF timeout slot. Pure: the caller owns the
 * slot word and performs the timer call.
 *
 * A slot value of 0 means nothing is armed. The nearest deadline owns the
 * slot; a later one never displaces it.
 */

/*
 * Offer an absolute deadline at time now_ns. A deadline at or before now is
 * moved to now + 1 (saturating at UINT64_MAX) so it fires on the next tick. If
 * the slot is empty or the deadline is earlier than the armed one, store it in
 * *slot, write the relative timeout to *relative_ns and return true: the
 * caller must then program the timer. Otherwise leave both outputs unchanged
 * and return false. slot and relative_ns must be non-null.
 */
[[__nodiscard__]] static inline bool
runtime_timer_slot_update(uint64_t *slot, uint64_t now_ns, uint64_t deadline_ns,
                          uint64_t *relative_ns) {
  if (deadline_ns <= now_ns) {
    deadline_ns = now_ns == UINT64_MAX ? UINT64_MAX : now_ns + 1;
  }
  if (*slot != 0 && deadline_ns >= *slot) {
    return false;
  }
  *slot = deadline_ns;
  *relative_ns = deadline_ns - now_ns;
  return true;
}

#endif
