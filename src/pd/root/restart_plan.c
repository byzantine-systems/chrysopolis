#include "restart_plan.h"

/* What an action does to the child's record. */
typedef enum : uint8_t {
  outcome_none,
  outcome_charge,
  outcome_stop,
  outcome_count,
} outcome;

/* The child machine, one row per root_child_state. A gone child absorbs
 * every outcome, which makes the absorption law hold by construction. */
static constexpr root_child_state
    child_next[root_child_gone + 1][outcome_count] = {
        [root_child_live] =
            {
                [outcome_none] = root_child_live,
                [outcome_charge] = root_child_live,
                [outcome_stop] = root_child_gone,
            },
        [root_child_gone] =
            {
                [outcome_none] = root_child_gone,
                [outcome_charge] = root_child_gone,
                [outcome_stop] = root_child_gone,
            },
};

static outcome outcome_of(root_restart_action action) {
  switch (action) {
  case root_restart_resume:
    return outcome_charge;
  case root_restart_giveup_budget_exhausted:
  case root_restart_giveup_window_exhausted:
  case root_restart_giveup_lifetime_exhausted:
  case root_restart_giveup_no_entry:
    return outcome_stop;
  case root_restart_ignore_gone:
    break;
  }
  return outcome_none;
}

static unsigned int min_u(unsigned int a, unsigned int b) {
  return a < b ? a : b;
}

/* Which selector decides. Chosen per call from the inputs, so a clock lost
 * mid-incarnation moves timed to lifetime_fallback and never back. */
typedef enum : uint8_t {
  selector_lifetime,
  selector_timed,
  selector_lifetime_fallback,
} selector;

static selector select_of(const root_restart_inputs *in) {
  if (!in->timed.enabled) {
    return selector_lifetime;
  }
  return in->clock_available && in->timed.policy.capacity != 0
             ? selector_timed
             : selector_lifetime_fallback;
}

unsigned int root_restart_ceiling(const root_spec_state *spec,
                                  const root_restart_inputs *in) {
  const unsigned int budget =
      root_spec_budget(spec, in->child, in->compiled, in->hard_max);
  if (!in->timed.enabled) {
    return budget;
  }
  const unsigned int limit = in->timed.policy.capacity != 0
                                 ? in->timed.policy.lifetime_limit
                                 : in->timed.lifetime_limit;
  return min_u(budget, limit);
}

root_restart_plan_result root_restart_plan(const root_spec_state *spec,
                                           const root_child_record *record,
                                           const root_restart_inputs *in) {
  root_restart_plan_result out = {.next = *record};
  const unsigned int budget =
      root_spec_budget(spec, in->child, in->compiled, in->hard_max);
  bool charged = false;
  switch (select_of(in)) {
  case selector_lifetime:
    out.action = root_restart_decide(record, budget, in->entry);
    break;
  case selector_lifetime_fallback:
    /* No usable clock: a lifetime-only ceiling, which never refills. */
    out.action = root_restart_decide(
        record, min_u(budget, in->timed.lifetime_limit), in->entry);
    break;
  case selector_timed: {
    /* The spec may lower the test ceiling, never raise it. */
    root_budget_policy policy = in->timed.policy;
    policy.lifetime_limit = min_u(policy.lifetime_limit, budget);
    const root_timed_decision decision =
        root_restart_decide_at(record, &policy, in->entry, in->now_ticks);
    if (decision.error != root_timed_valid) {
      out.policy_invalid = true;
      out.action = root_restart_giveup_budget_exhausted;
      break;
    }
    out.action = decision.action;
    if (out.action == root_restart_resume) {
      out.next = decision.next;
      charged = true;
    }
    break;
  }
  }
  const outcome effect = outcome_of(out.action);
  if (effect == outcome_charge && !charged) {
    root_record_charge(&out.next);
  }
  /* An undeclared state is treated as gone, the safe end of the machine. */
  const root_child_state from =
      record->state == root_child_live ? root_child_live : root_child_gone;
  out.next.state = child_next[from][effect];
  return out;
}
