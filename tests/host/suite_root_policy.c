/* root_policy.h: restart budgets, give-up reasons and channel routing. */
#include "check.h"

#include "root_policy.h"

#include <limits.h>
#include <string.h>

enum : size_t { max_children = 8 };

static constexpr uint64_t entry = 0x200000;

static void test_decision_order(void) {
  unsigned int counts[max_children] = {};

  CHECK(root_restart_decide(0, max_children, counts, 3, entry) ==
        root_restart_resume);
  CHECK(root_restart_decide(max_children - 1, max_children, counts, 3, entry) ==
        root_restart_resume);

  /* The range check comes first: the count table is never read past its end,
   * and neither the budget nor the entry can rescue a bad id. */
  CHECK(root_restart_decide(max_children, max_children, counts, 3, entry) ==
        root_restart_giveup_out_of_range);
  CHECK(root_restart_decide(SIZE_MAX, max_children, counts, 3, 0) ==
        root_restart_giveup_out_of_range);

  /* Budget before entry, so an exhausted child without an entry reports the
   * budget. */
  counts[2] = 3;
  CHECK(root_restart_decide(2, max_children, counts, 3, 0) ==
        root_restart_giveup_budget_exhausted);
  CHECK(root_restart_decide(1, max_children, counts, 3, 0) ==
        root_restart_giveup_no_entry);

  /* A zero budget gives up on the first fault. */
  CHECK(root_restart_decide(1, max_children, counts, 0, entry) ==
        root_restart_giveup_budget_exhausted);
}

/* Charge-then-resume until the budget runs out, the loop fault() and
 * notified() drive between them. A re-entrant fault sees the charged count. */
static void test_budget_converges(void) {
  for (unsigned int budget = 0; budget < 12; budget++) {
    unsigned int counts[max_children] = {};
    unsigned int resumes = 0;
    for (size_t event = 0; event < 20; event++) {
      if (root_restart_decide(4, max_children, counts, budget, entry) !=
          root_restart_resume) {
        break;
      }
      counts[4]++;
      resumes++;
    }
    CHECK(resumes == budget);
    CHECK(root_restart_decide(4, max_children, counts, budget, entry) ==
          root_restart_giveup_budget_exhausted);
    /* Budgets are per child. */
    CHECK(budget == 0 || root_restart_decide(5, max_children, counts, budget,
                                             entry) == root_restart_resume);
  }
}

static void test_giveup_reasons(void) {
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_out_of_range),
               "out-of-range") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_budget_exhausted),
               "budget-exhausted") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_no_entry),
               "no-restart-entry") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_resume), "") == 0);
}

/* The layout runtime-abi.json uses today: debug 0..3, fault 4..7. */
static void test_notify_routing(void) {
  static constexpr unsigned int debug_max = 3;
  static constexpr unsigned int fault_first = 4;
  static constexpr unsigned int fault_max = 7;

  for (unsigned int ch = 0; ch <= debug_max; ch++) {
    size_t index = SIZE_MAX;
    CHECK(root_notify_route(ch, debug_max, fault_first, fault_max, &index) ==
          root_notify_debug_restart);
    CHECK_EQ_U64(index, ch);
  }
  for (unsigned int ch = fault_first; ch <= fault_max; ch++) {
    size_t index = SIZE_MAX;
    CHECK(root_notify_route(ch, debug_max, fault_first, fault_max, &index) ==
          root_notify_fault_inject);
    CHECK_EQ_U64(index, ch - fault_first);
  }
  static const unsigned int unexpected[] = {8, 9, 62, UINT_MAX};
  for (size_t i = 0; i < sizeof(unexpected) / sizeof(unexpected[0]); i++) {
    size_t index = SIZE_MAX;
    CHECK(root_notify_route(unexpected[i], debug_max, fault_first, fault_max,
                            &index) == root_notify_unexpected);
    CHECK_EQ_U64(index, SIZE_MAX);
  }
}

int main(void) {
  test_decision_order();
  test_budget_converges();
  test_giveup_reasons();
  test_notify_routing();
  return check_finish("root_policy");
}
