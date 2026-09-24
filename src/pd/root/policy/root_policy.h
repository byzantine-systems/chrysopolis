#ifndef CHRYSOPOLIS_ROOT_POLICY_H
#define CHRYSOPOLIS_ROOT_POLICY_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Root's restart and notification decisions. Header-only and pure so Root
 * stays free of libc and sDDF: every limit arrives as a parameter, and the
 * caller performs the Microkit calls and logging.
 */

/*
 * Microkit child and channel ids are both plain unsigned ints, in separate id
 * spaces (BASE_TCB_CAP versus BASE_OUTPUT_NOTIFICATION_CAP). Root wraps them
 * so one cannot be passed where the other is expected. A root_child is only
 * produced by root_child_from_raw, so holding one means the id was validated.
 */
typedef struct {
  unsigned int value;
} root_child;

typedef struct {
  unsigned int value;
} root_channel;

/* Bits available in a known-child mask. */
static constexpr unsigned int root_child_mask_bits = 64;

/*
 * Accept raw as a child id when it is below max_children and its bit is set
 * in known_mask. On success *out receives the id; on failure *out is not
 * written and the caller must not invoke any capability derived from raw.
 */
[[__nodiscard__]] static inline bool root_child_from_raw(unsigned int raw,
                                                         size_t max_children,
                                                         uint64_t known_mask,
                                                         root_child *out) {
  if (raw >= max_children || raw >= root_child_mask_bits ||
      ((known_mask >> raw) & 1u) == 0) {
    return false;
  }
  *out = (root_child){.value = raw};
  return true;
}

typedef enum : uint8_t {
  root_child_live,
  /* Root stopped the child for good. Nothing restarts or resumes it. */
  root_child_gone,
} root_child_state;

/* Per-child restart accounting. Lifetime never decays. A zero-initialised
 * record is live and has never been restarted. The timestamp-valid bit lets
 * tick zero be the first real charge. */
typedef struct {
  unsigned int lifetime_count;
  unsigned int window_count;
  uint64_t last_charge_ticks;
  bool last_charge_valid;
  root_child_state state;
} root_child_record;

typedef enum {
  root_restart_resume,
  root_restart_giveup_budget_exhausted,
  root_restart_giveup_window_exhausted,
  root_restart_giveup_lifetime_exhausted,
  /* The image carries no entry point for this child. */
  root_restart_giveup_no_entry,
  /* The child is already gone: make no Microkit call at all. */
  root_restart_ignore_gone,
} root_restart_action;

/*
 * Decide what to do with a child that faulted or was asked to restart. The
 * checks run in this order: a gone child is ignored, then the count is held
 * against budget, then the entry must be non-zero. On resume the caller
 * charges one restart (root_record_charge) before resuming the child, so a
 * child that faults again at once observes the charged count. On a give-up
 * the caller marks the record gone (root_record_give_up) before stopping the
 * child.
 */
[[__nodiscard__]] static inline root_restart_action
root_restart_decide(const root_child_record *record, unsigned int budget,
                    uint64_t entry) {
  if (record->state == root_child_gone) {
    return root_restart_ignore_gone;
  }
  if (record->lifetime_count >= budget) {
    return root_restart_giveup_budget_exhausted;
  }
  if (entry == 0) {
    return root_restart_giveup_no_entry;
  }
  return root_restart_resume;
}

/* Charge one restart. Only valid after root_restart_resume, which bounds the
 * count below the budget, so the increment cannot wrap. */
static inline void root_record_charge(root_child_record *record) {
  record->lifetime_count++;
}

/* Mark a child gone. Idempotent. */
static inline void root_record_give_up(root_child_record *record) {
  record->state = root_child_gone;
}

/* The reason logged for a give-up action. Other actions have no reason. */
[[__nodiscard__]] static inline const char *
root_restart_giveup_reason(root_restart_action action) {
  switch (action) {
  case root_restart_giveup_budget_exhausted:
    return "budget-exhausted";
  case root_restart_giveup_window_exhausted:
    return "window-exhausted";
  case root_restart_giveup_lifetime_exhausted:
    return "lifetime-exhausted";
  case root_restart_giveup_no_entry:
    return "no-restart-entry";
  case root_restart_resume:
  case root_restart_ignore_gone:
    break;
  }
  return "";
}

/* Root has no timer notification. It samples this counter at init and on
 * events; a backwards sample permanently disables time-based decisions. */
typedef enum {
  root_clock_ok,
  root_clock_bad_frequency,
  root_clock_regressed,
  root_clock_bad_interval,
  root_clock_interval_overflow,
} root_clock_result;

typedef struct {
  uint64_t ticks_per_ms;
  uint64_t last_ticks;
  bool seen;
  bool available;
} root_clock;

[[__nodiscard__]] static inline root_clock_result
root_clock_init(root_clock *clock, uint64_t frequency_hz, uint64_t minimum_hz,
                uint64_t maximum_hz) {
  *clock = (root_clock){};
  if (minimum_hz < 1000 || minimum_hz > maximum_hz ||
      frequency_hz < minimum_hz || frequency_hz > maximum_hz) {
    return root_clock_bad_frequency;
  }
  /* Round up so a policy window never refills earlier than requested. */
  clock->ticks_per_ms = frequency_hz / 1000 + (frequency_hz % 1000 != 0);
  clock->available = true;
  return root_clock_ok;
}

[[__nodiscard__]] static inline root_clock_result
root_clock_observe(root_clock *clock, uint64_t ticks) {
  if (!clock->available) {
    return root_clock_bad_frequency;
  }
  if (clock->ticks_per_ms == 0) {
    clock->available = false;
    return root_clock_bad_frequency;
  }
  if (clock->seen && ticks < clock->last_ticks) {
    clock->available = false;
    return root_clock_regressed;
  }
  clock->seen = true;
  clock->last_ticks = ticks;
  return root_clock_ok;
}

[[__nodiscard__]] static inline root_clock_result
root_clock_interval(const root_clock *clock, uint64_t milliseconds,
                    uint64_t *ticks) {
  if (!clock->available || clock->ticks_per_ms == 0) {
    return root_clock_bad_frequency;
  }
  if (milliseconds == 0) {
    return root_clock_bad_interval;
  }
  if (milliseconds > UINT64_MAX / clock->ticks_per_ms) {
    return root_clock_interval_overflow;
  }
  *ticks = milliseconds * clock->ticks_per_ms;
  return root_clock_ok;
}

/* One event consumes one window and one lifetime token. The caller commits
 * next before invoking Microkit, so an immediate refault sees the charge. */
typedef struct {
  unsigned int capacity;
  unsigned int lifetime_limit;
  uint64_t leak_interval_ticks;
} root_budget_policy;

typedef enum {
  root_timed_valid,
  root_timed_invalid_policy,
  root_timed_invalid_record,
  root_timed_clock_regressed,
} root_timed_error;

typedef struct {
  root_timed_error error;
  root_restart_action action;
  root_child_record next;
} root_timed_decision;

[[__nodiscard__]] static inline root_timed_decision
root_restart_decide_at(const root_child_record *record,
                       const root_budget_policy *policy, uint64_t entry,
                       uint64_t now_ticks) {
  root_timed_decision result = {
      .error = root_timed_valid,
      .action = root_restart_ignore_gone,
      .next = *record,
  };
  if (record->state == root_child_gone) {
    return result;
  }
  if (policy->capacity == 0 || policy->lifetime_limit == 0 ||
      policy->leak_interval_ticks == 0) {
    result.error = root_timed_invalid_policy;
    return result;
  }
  if (record->window_count > policy->capacity ||
      record->last_charge_valid != (record->window_count != 0)) {
    result.error = root_timed_invalid_record;
    return result;
  }
  if (record->last_charge_valid && now_ticks < record->last_charge_ticks) {
    result.error = root_timed_clock_regressed;
    return result;
  }
  if (record->lifetime_count >= policy->lifetime_limit) {
    result.action = root_restart_giveup_lifetime_exhausted;
    return result;
  }

  if (record->last_charge_valid) {
    const uint64_t elapsed = now_ticks - record->last_charge_ticks;
    const uint64_t leaked = elapsed / policy->leak_interval_ticks;
    if (leaked >= record->window_count) {
      result.next.window_count = 0;
      result.next.last_charge_valid = false;
    } else if (leaked != 0) {
      result.next.window_count -= (unsigned int)leaked;
      result.next.last_charge_ticks += leaked * policy->leak_interval_ticks;
    }
  }
  if (result.next.window_count >= policy->capacity) {
    result.action = root_restart_giveup_window_exhausted;
    return result;
  }
  if (entry == 0) {
    result.action = root_restart_giveup_no_entry;
    return result;
  }
  if (result.next.window_count == 0) {
    result.next.last_charge_ticks = now_ticks;
    result.next.last_charge_valid = true;
  }
  result.next.window_count++;
  result.next.lifetime_count++;
  result.action = root_restart_resume;
  return result;
}

/*
 * The test-only notification layout: count debug-restart channels starting at
 * debug_first and count fault-injection channels starting at fault_first,
 * both in driver-class order. Root checks its constants against the same
 * rules with static assertions.
 */
typedef struct {
  unsigned int debug_first;
  unsigned int fault_first;
  unsigned int count;
} root_channel_layout;

/* True when a group of count channels from first fits below max_channels. */
[[__nodiscard__]] static inline bool
root_channel_group_fits(unsigned int first, unsigned int count,
                        unsigned int max_channels) {
  return first < max_channels && count <= max_channels - first;
}

/* True when ch lies in the group of count channels from first. */
[[__nodiscard__]] static inline bool
root_channel_in_group(unsigned int ch, unsigned int first, unsigned int count) {
  return ch >= first && ch - first < count;
}

/*
 * True when both groups are non-empty, fit below max_channels, do not
 * overlap, and neither contains gone (pass a value at or above max_channels
 * when there is no give-up channel).
 */
[[__nodiscard__]] static inline bool
root_channel_layout_valid(const root_channel_layout *layout,
                          unsigned int max_channels, unsigned int gone) {
  if (layout->count == 0 ||
      !root_channel_group_fits(layout->debug_first, layout->count,
                               max_channels) ||
      !root_channel_group_fits(layout->fault_first, layout->count,
                               max_channels)) {
    return false;
  }
  const unsigned int debug_end = layout->debug_first + layout->count;
  const unsigned int fault_end = layout->fault_first + layout->count;
  if (layout->debug_first < fault_end && layout->fault_first < debug_end) {
    return false;
  }
  return !root_channel_in_group(gone, layout->debug_first, layout->count) &&
         !root_channel_in_group(gone, layout->fault_first, layout->count);
}

typedef enum {
  /* Restart a healthy driver: *index is the driver-class index. */
  root_notify_debug_restart,
  /* Resume a driver at address 0: *index is the driver-class index. */
  root_notify_fault_inject,
  root_notify_unexpected,
} root_notify_kind;

/*
 * Route a notification channel through a layout that passed
 * root_channel_layout_valid. Both groups are bounded below and above. *index
 * is written only for the two routed kinds; index must be non-null.
 */
[[__nodiscard__]] static inline root_notify_kind
root_notify_route(root_channel ch, const root_channel_layout *layout,
                  size_t *index) {
  if (root_channel_in_group(ch.value, layout->debug_first, layout->count)) {
    *index = ch.value - layout->debug_first;
    return root_notify_debug_restart;
  }
  if (root_channel_in_group(ch.value, layout->fault_first, layout->count)) {
    *index = ch.value - layout->fault_first;
    return root_notify_fault_inject;
  }
  return root_notify_unexpected;
}

#endif
