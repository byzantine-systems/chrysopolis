#ifndef CHRYSOPOLIS_ROOT_RESTART_PLAN_H
#define CHRYSOPOLIS_ROOT_RESTART_PLAN_H 1

/* The decision half of Root's restart path: which ceiling applies, which
 * selector decides, and the record that results. Pure: no globals,
 * allocation, logging or Microkit calls, so Root links it and tests/host
 * checks its laws. main.c performs the restart or stop it decides and
 * publishes the outcome. Not thread-safe: Root's callbacks are serialized.
 *
 * Laws (suite_root_fallback.c):
 *   identity    no spec, or only rejected ones, gives the compiled budget
 *   cap         the effective budget is min(spec ?: compiled, hard_max)
 *   absorption  a gone record yields ignore_gone and stays unchanged
 *   monotone    lifetime_count never decreases
 * A spec may raise a budget up to hard_max, so it does not only lower one. */

#include "root_policy.h"
#include "spec.h"

#include <stdbool.h>
#include <stdint.h>

/* The time-based test selector. Production passes enabled = false and keeps
 * the lifetime-only decision. policy.capacity == 0 means the clock interval
 * was unavailable at init, which selects lifetime_limit as a plain ceiling. */
typedef struct {
  bool enabled;
  root_budget_policy policy;
  unsigned int lifetime_limit;
} root_restart_timed;

typedef struct {
  unsigned int child;
  /* This child's compiled lifetime budget and the ABI's hard maximum. */
  unsigned int compiled;
  unsigned int hard_max;
  uint64_t entry;
  root_restart_timed timed;
  /* The clock after the adapter's observation of now_ticks. */
  bool clock_available;
  uint64_t now_ticks;
} root_restart_inputs;

typedef struct {
  root_restart_action action;
  /* The record to commit before acting: charged on resume, gone on a
   * give-up, unchanged otherwise. */
  root_child_record next;
  /* The timed selector rejected the record or policy. The adapter turns the
   * clock off for good; the action is already a give-up. */
  bool policy_invalid;
} root_restart_plan_result;

/* The lifetime ceiling the plan applies to this child, for status rows.
 * spec and in must be non-null. */
[[__nodiscard__]] unsigned int
root_restart_ceiling(const root_spec_state *spec,
                     const root_restart_inputs *in);

/* Decides one restart request. The checks keep root_restart_decide's order:
 * gone, then the budget, then the entry. spec, record and in must be
 * non-null. */
[[__nodiscard__]] root_restart_plan_result
root_restart_plan(const root_spec_state *spec, const root_child_record *record,
                  const root_restart_inputs *in);

#endif
