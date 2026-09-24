/* Root's root_policy.h: child id validation, restart budgets, the terminal gone
 * state, give-up reasons and channel routing. */
#include "check.h"

#include "root_policy.h"

#include <limits.h>
#include <string.h>

enum : size_t { max_children = 62 };

static constexpr uint64_t entry = 0x200000;

static uint64_t bit(unsigned int id) { return UINT64_C(1) << id; }

static void test_child_from_raw(void) {
  const uint64_t known = bit(0) | bit(5) | bit(61);
  root_child child = {.value = 999};

  CHECK(root_child_from_raw(0, max_children, known, &child));
  CHECK_EQ_U64(child.value, 0);
  CHECK(root_child_from_raw(61, max_children, known, &child));
  CHECK_EQ_U64(child.value, 61);

  /* In range but not a child: rejected, and *out untouched. */
  child.value = 999;
  CHECK(!root_child_from_raw(1, max_children, known, &child));
  CHECK_EQ_U64(child.value, 999);

  /* The table bound applies even when the mask says yes. */
  CHECK(!root_child_from_raw(62, max_children, bit(62), &child));
  CHECK(root_child_from_raw(62, 63, bit(62), &child));
  CHECK(root_child_from_raw(63, SIZE_MAX, bit(63), &child));

  /* Ids at or past the mask width never shift out of range. */
  static const unsigned int too_big[] = {64, 65, 255, UINT_MAX};
  for (size_t i = 0; i < sizeof(too_big) / sizeof(too_big[0]); i++) {
    child.value = 999;
    CHECK(!root_child_from_raw(too_big[i], SIZE_MAX, UINT64_MAX, &child));
    CHECK_EQ_U64(child.value, 999);
  }

  CHECK(!root_child_from_raw(0, 0, UINT64_MAX, &child));
  CHECK(!root_child_from_raw(0, max_children, 0, &child));
}

static void test_decision_order(void) {
  root_child_record record = {};

  CHECK(root_restart_decide(&record, 3, entry) == root_restart_resume);

  /* Budget before entry, so an exhausted child without an entry reports the
   * budget. */
  record.lifetime_count = 3;
  CHECK(root_restart_decide(&record, 3, 0) ==
        root_restart_giveup_budget_exhausted);
  record.lifetime_count = 2;
  CHECK(root_restart_decide(&record, 3, 0) == root_restart_giveup_no_entry);

  /* A zero budget gives up on the first fault. */
  record.lifetime_count = 0;
  CHECK(root_restart_decide(&record, 0, entry) ==
        root_restart_giveup_budget_exhausted);

  /* Gone wins over everything, including a fresh budget and a valid entry. */
  root_record_give_up(&record);
  CHECK(root_restart_decide(&record, 3, entry) == root_restart_ignore_gone);
  CHECK(root_restart_decide(&record, UINT_MAX, entry) ==
        root_restart_ignore_gone);
  CHECK(root_restart_decide(&record, 0, 0) == root_restart_ignore_gone);
}

/* Charge-then-resume until the budget runs out, then give up and keep
 * sending requests: the loop fault() and notified() drive between them. */
static void test_budget_then_gone(void) {
  for (unsigned int budget = 0; budget < 12; budget++) {
    root_child_record records[3] = {};
    unsigned int resumes = 0;
    unsigned int giveups = 0;
    unsigned int ignored = 0;
    for (size_t event = 0; event < 40; event++) {
      switch (root_restart_decide(&records[1], budget, entry)) {
      case root_restart_resume:
        root_record_charge(&records[1]);
        resumes++;
        break;
      case root_restart_giveup_budget_exhausted:
      case root_restart_giveup_window_exhausted:
      case root_restart_giveup_lifetime_exhausted:
      case root_restart_giveup_no_entry:
        root_record_give_up(&records[1]);
        giveups++;
        break;
      case root_restart_ignore_gone:
        ignored++;
        break;
      }
    }
    CHECK(resumes == budget);
    /* Exactly one give-up however many requests follow, so the dependent is
     * notified once. */
    CHECK(giveups == 1);
    CHECK(ignored == 40 - budget - 1);
    CHECK_EQ_U64(records[1].lifetime_count, budget);
    /* Budgets and states are per child. */
    CHECK(records[0].state == root_child_live &&
          records[0].lifetime_count == 0);
    CHECK(records[2].state == root_child_live &&
          records[2].lifetime_count == 0);
    CHECK(budget == 0 || root_restart_decide(&records[2], budget, entry) ==
                             root_restart_resume);
  }
}

static void test_giveup_reasons(void) {
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_budget_exhausted),
               "budget-exhausted") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_no_entry),
               "no-restart-entry") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_giveup_window_exhausted),
               "window-exhausted") == 0);
  CHECK(
      strcmp(root_restart_giveup_reason(root_restart_giveup_lifetime_exhausted),
             "lifetime-exhausted") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_resume), "") == 0);
  CHECK(strcmp(root_restart_giveup_reason(root_restart_ignore_gone), "") == 0);
}

static void test_clock(void) {
  root_clock clock = {};
  uint64_t interval = 99;
  CHECK(root_clock_init(&clock, 0, 1000000, 1000000000) ==
        root_clock_bad_frequency);
  CHECK(!clock.available);
  CHECK(root_clock_observe(&clock, 1) == root_clock_bad_frequency);
  CHECK(root_clock_interval(&clock, 1, &interval) == root_clock_bad_frequency);
  CHECK_EQ_U64(interval, 99);
  CHECK(root_clock_init(&clock, 999999, 1000000, 1000000000) ==
        root_clock_bad_frequency);
  CHECK(root_clock_init(&clock, 1000000001, 1000000, 1000000000) ==
        root_clock_bad_frequency);
  CHECK(root_clock_init(&clock, 1000000, 1000000, 1000000000) == root_clock_ok);
  CHECK_EQ_U64(clock.ticks_per_ms, 1000);
  CHECK(root_clock_init(&clock, 1000000000, 1000000, 1000000000) ==
        root_clock_ok);
  CHECK_EQ_U64(clock.ticks_per_ms, 1000000);
  CHECK(root_clock_init(&clock, 62500000, 1000000, 1000000000) ==
        root_clock_ok);
  CHECK_EQ_U64(clock.ticks_per_ms, 62500);
  CHECK(root_clock_interval(&clock, 0, &interval) == root_clock_bad_interval);
  CHECK(root_clock_interval(&clock, 2000, &interval) == root_clock_ok);
  CHECK_EQ_U64(interval, 125000000);
  CHECK(root_clock_interval(&clock, UINT64_MAX / 62500, &interval) ==
        root_clock_ok);
  CHECK(root_clock_interval(&clock, UINT64_MAX / 62500 + 1, &interval) ==
        root_clock_interval_overflow);
  CHECK(root_clock_observe(&clock, 0) == root_clock_ok);
  CHECK(root_clock_observe(&clock, 0) == root_clock_ok);
  CHECK(root_clock_observe(&clock, UINT64_MAX - 1) == root_clock_ok);
  CHECK(root_clock_observe(&clock, 1) == root_clock_regressed);
  CHECK(!clock.available);
  CHECK(root_clock_interval(&clock, 1, &interval) == root_clock_bad_frequency);

  /* Non-integral milliseconds round up, so a leak cannot arrive early. */
  CHECK(root_clock_init(&clock, 1000001, 1000000, 1000000000) == root_clock_ok);
  CHECK_EQ_U64(clock.ticks_per_ms, 1001);
}

static void test_timed_budget(void) {
  const root_budget_policy policy = {
      .capacity = 2, .lifetime_limit = 4, .leak_interval_ticks = 10};
  root_child_record record = {};
  root_timed_decision decision =
      root_restart_decide_at(&record, &policy, entry, 0);
  CHECK(decision.error == root_timed_valid);
  CHECK(decision.action == root_restart_resume);
  record = decision.next;
  CHECK_EQ_U64(record.lifetime_count, 1);
  CHECK_EQ_U64(record.window_count, 1);
  CHECK(record.last_charge_valid);
  CHECK_EQ_U64(record.last_charge_ticks, 0);

  decision = root_restart_decide_at(&record, &policy, entry, 9);
  CHECK(decision.action == root_restart_resume);
  record = decision.next;
  CHECK_EQ_U64(record.window_count, 2);

  /* No elapsed interval yet: the next fault exhausts the window. */
  decision = root_restart_decide_at(&record, &policy, entry, 9);
  CHECK(decision.action == root_restart_giveup_window_exhausted);
  CHECK_EQ_U64(decision.next.lifetime_count, 2);

  /* At the exact boundary one unit leaks, preserving the remaining fraction. */
  decision = root_restart_decide_at(&record, &policy, entry, 10);
  CHECK(decision.action == root_restart_resume);
  record = decision.next;
  CHECK_EQ_U64(record.lifetime_count, 3);
  CHECK_EQ_U64(record.window_count, 2);
  CHECK_EQ_U64(record.last_charge_ticks, 10);

  decision = root_restart_decide_at(&record, &policy, entry, 19);
  CHECK(decision.action == root_restart_giveup_window_exhausted);
  decision = root_restart_decide_at(&record, &policy, entry, 20);
  CHECK(decision.action == root_restart_resume);
  record = decision.next;
  CHECK_EQ_U64(record.lifetime_count, 4);
  CHECK_EQ_U64(record.window_count, 2);
  decision = root_restart_decide_at(&record, &policy, entry, 1000);
  CHECK(decision.action == root_restart_giveup_lifetime_exhausted);
  CHECK_EQ_U64(decision.next.lifetime_count, 4);
  root_record_give_up(&record);
  CHECK(root_restart_decide_at(&record, &policy, entry, 1001).action ==
        root_restart_ignore_gone);
}

static void test_timed_errors_and_idle(void) {
  const root_budget_policy policy = {
      .capacity = 2, .lifetime_limit = 5, .leak_interval_ticks = 10};
  root_child_record record = {};
  root_timed_decision decision =
      root_restart_decide_at(&record, &policy, entry, 3);
  record = decision.next;
  decision = root_restart_decide_at(&record, &policy, entry, UINT64_MAX);
  CHECK(decision.action == root_restart_resume);
  record = decision.next;
  CHECK_EQ_U64(record.window_count, 1);
  CHECK_EQ_U64(record.last_charge_ticks, UINT64_MAX);
  CHECK_EQ_U64(record.lifetime_count, 2);

  decision = root_restart_decide_at(&record, &policy, entry, 2);
  CHECK(decision.error == root_timed_clock_regressed);
  CHECK_EQ_U64(decision.next.lifetime_count, 2);
  CHECK(root_restart_decide(&record, 2, entry) ==
        root_restart_giveup_budget_exhausted);

  root_budget_policy bad = policy;
  bad.leak_interval_ticks = 0;
  CHECK(root_restart_decide_at(&record, &bad, entry, UINT64_MAX).error ==
        root_timed_invalid_policy);
  bad = policy;
  bad.capacity = 0;
  CHECK(root_restart_decide_at(&record, &bad, entry, UINT64_MAX).error ==
        root_timed_invalid_policy);
  record.window_count = 3;
  CHECK(root_restart_decide_at(&record, &policy, entry, UINT64_MAX).error ==
        root_timed_invalid_record);
  record = (root_child_record){};
  record.window_count = 1;
  CHECK(root_restart_decide_at(&record, &policy, entry, 1).error ==
        root_timed_invalid_record);
  record = (root_child_record){};
  record.last_charge_valid = true;
  CHECK(root_restart_decide_at(&record, &policy, entry, 1).error ==
        root_timed_invalid_record);
  record = (root_child_record){};
  CHECK(root_restart_decide_at(&record, &policy, 0, 1).action ==
        root_restart_giveup_no_entry);
  CHECK_EQ_U64(record.lifetime_count, 0);
}

enum : unsigned int { max_channels = 62, no_gone = UINT_MAX };

static void test_layout_validity(void) {
  /* The layout system_abi.zig uses today. */
  root_channel_layout layout = {.debug_first = 0, .fault_first = 4, .count = 4};
  CHECK(root_channel_layout_valid(&layout, max_channels, 10));
  CHECK(root_channel_layout_valid(&layout, max_channels, no_gone));

  /* The give-up channel may touch a group but not sit inside one. */
  CHECK(root_channel_layout_valid(&layout, max_channels, 8));
  CHECK(!root_channel_layout_valid(&layout, max_channels, 0));
  CHECK(!root_channel_layout_valid(&layout, max_channels, 3));
  CHECK(!root_channel_layout_valid(&layout, max_channels, 7));

  /* Touching groups are fine, one channel of overlap is not, either way
   * round. */
  layout =
      (root_channel_layout){.debug_first = 4, .fault_first = 0, .count = 4};
  CHECK(root_channel_layout_valid(&layout, max_channels, no_gone));
  layout =
      (root_channel_layout){.debug_first = 0, .fault_first = 3, .count = 4};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));
  layout =
      (root_channel_layout){.debug_first = 3, .fault_first = 0, .count = 4};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));
  layout =
      (root_channel_layout){.debug_first = 9, .fault_first = 9, .count = 1};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));

  /* The top edge: the last group may end exactly at max_channels. */
  layout =
      (root_channel_layout){.debug_first = 54, .fault_first = 58, .count = 4};
  CHECK(root_channel_layout_valid(&layout, max_channels, no_gone));
  layout =
      (root_channel_layout){.debug_first = 54, .fault_first = 59, .count = 4};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));
  layout = (root_channel_layout){
      .debug_first = 0, .fault_first = UINT_MAX, .count = 4};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));
  layout = (root_channel_layout){
      .debug_first = 4, .fault_first = 0, .count = UINT_MAX};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));

  layout =
      (root_channel_layout){.debug_first = 0, .fault_first = 4, .count = 0};
  CHECK(!root_channel_layout_valid(&layout, max_channels, no_gone));
}

static void check_routes(const root_channel_layout *layout) {
  CHECK(root_channel_layout_valid(layout, max_channels, no_gone));
  for (unsigned int ch = 0; ch < max_channels + 2; ch++) {
    size_t index = SIZE_MAX;
    const root_notify_kind kind =
        root_notify_route((root_channel){.value = ch}, layout, &index);
    if (ch >= layout->debug_first && ch < layout->debug_first + layout->count) {
      CHECK(kind == root_notify_debug_restart);
      CHECK_EQ_U64(index, ch - layout->debug_first);
    } else if (ch >= layout->fault_first &&
               ch < layout->fault_first + layout->count) {
      CHECK(kind == root_notify_fault_inject);
      CHECK_EQ_U64(index, ch - layout->fault_first);
    } else {
      CHECK(kind == root_notify_unexpected);
      CHECK_EQ_U64(index, SIZE_MAX);
    }
  }
  size_t index = SIZE_MAX;
  CHECK(root_notify_route((root_channel){.value = UINT_MAX}, layout, &index) ==
        root_notify_unexpected);
  CHECK_EQ_U64(index, SIZE_MAX);
}

static void test_notify_routing(void) {
  check_routes(
      &(root_channel_layout){.debug_first = 0, .fault_first = 4, .count = 4});
  /* A debug group that does not start at 0 has a lower bound: channels below
   * it are unexpected rather than debug index ch. */
  check_routes(
      &(root_channel_layout){.debug_first = 20, .fault_first = 8, .count = 4});
  check_routes(
      &(root_channel_layout){.debug_first = 58, .fault_first = 1, .count = 4});
}

int main(void) {
  test_child_from_raw();
  test_decision_order();
  test_budget_then_gone();
  test_giveup_reasons();
  test_clock();
  test_timed_budget();
  test_timed_errors_and_idle();
  test_layout_validity();
  test_notify_routing();
  return check_finish("root_policy");
}
