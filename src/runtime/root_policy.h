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

typedef enum {
  root_restart_resume,
  /* The child id has no slot in the budget table. */
  root_restart_giveup_out_of_range,
  root_restart_giveup_budget_exhausted,
  /* The image carries no entry point for this child. */
  root_restart_giveup_no_entry,
} root_restart_action;

/*
 * Decide what to do with a child that faulted or was asked to restart. The
 * checks run in this order: child id in [0, max_children), then the child's
 * count against budget, then a non-zero entry. counts holds max_children
 * entries and is read only at a validated index. On resume the caller charges
 * one restart before resuming the child, so a child that faults again at once
 * observes the charged count.
 */
[[__nodiscard__]] static inline root_restart_action
root_restart_decide(size_t child, size_t max_children,
                    const unsigned int counts[static 1], unsigned int budget,
                    uint64_t entry) {
  if (child >= max_children) {
    return root_restart_giveup_out_of_range;
  }
  if (counts[child] >= budget) {
    return root_restart_giveup_budget_exhausted;
  }
  if (entry == 0) {
    return root_restart_giveup_no_entry;
  }
  return root_restart_resume;
}

/* The reason logged for a give-up action. Resume has no reason. */
[[__nodiscard__]] static inline const char *
root_restart_giveup_reason(root_restart_action action) {
  switch (action) {
  case root_restart_giveup_out_of_range:
    return "out-of-range";
  case root_restart_giveup_budget_exhausted:
    return "budget-exhausted";
  case root_restart_giveup_no_entry:
    return "no-restart-entry";
  case root_restart_resume:
    break;
  }
  return "";
}

typedef enum {
  /* Restart a healthy driver: *index is the debug-restart table index. */
  root_notify_debug_restart,
  /* Resume a driver at address 0: *index is the fault-injection index. */
  root_notify_fault_inject,
  root_notify_unexpected,
} root_notify_kind;

/*
 * Route a notification channel. Channels [0, debug_max] are debug restarts
 * indexed by channel, and [fault_first, fault_max] are fault injections
 * indexed from fault_first. Debug routing is checked first. *index is written
 * only for the two routed kinds; index must be non-null.
 */
[[__nodiscard__]] static inline root_notify_kind
root_notify_route(unsigned int ch, unsigned int debug_max,
                  unsigned int fault_first, unsigned int fault_max,
                  size_t *index) {
  if (ch <= debug_max) {
    *index = ch;
    return root_notify_debug_restart;
  }
  if (ch >= fault_first && ch <= fault_max) {
    *index = ch - fault_first;
    return root_notify_fault_inject;
  }
  return root_notify_unexpected;
}

#endif
