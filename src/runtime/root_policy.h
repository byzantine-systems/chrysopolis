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

/* Per-child restart accounting. A zero-initialised record is a live child
 * that has not been restarted. */
typedef struct {
  unsigned int count;
  root_child_state state;
} root_child_record;

typedef enum {
  root_restart_resume,
  root_restart_giveup_budget_exhausted,
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
  if (record->count >= budget) {
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
  record->count++;
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
  case root_restart_giveup_no_entry:
    return "no-restart-entry";
  case root_restart_resume:
  case root_restart_ignore_gone:
    break;
  }
  return "";
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
