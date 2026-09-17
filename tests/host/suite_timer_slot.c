/* runtime_timer_slot.h: the single sDDF timeout slot shared by all waiters. */
#include "check.h"

#include "runtime_timer_slot.h"

#include <stdint.h>

static void test_empty_slot_arms(void) {
  uint64_t slot = 0;
  uint64_t relative = UINT64_MAX;
  CHECK(runtime_timer_slot_update(&slot, 100, 150, &relative));
  CHECK_EQ_U64(slot, 150);
  CHECK_EQ_U64(relative, 50);
}

static void test_expired_deadline_fires_next_tick(void) {
  uint64_t slot = 0;
  uint64_t relative = 0;
  CHECK(runtime_timer_slot_update(&slot, 100, 100, &relative));
  CHECK_EQ_U64(slot, 101);
  CHECK_EQ_U64(relative, 1);

  slot = 0;
  CHECK(runtime_timer_slot_update(&slot, 100, 0, &relative));
  CHECK_EQ_U64(slot, 101);
  CHECK_EQ_U64(relative, 1);

  /* now + 1 saturates instead of wrapping to the empty-slot value. */
  slot = 0;
  CHECK(runtime_timer_slot_update(&slot, UINT64_MAX, 7, &relative));
  CHECK_EQ_U64(slot, UINT64_MAX);
  CHECK_EQ_U64(relative, 0);
}

static void test_nearest_deadline_owns_slot(void) {
  uint64_t slot = 0;
  uint64_t relative = 0;
  CHECK(runtime_timer_slot_update(&slot, 0, 500, &relative));

  /* A later or equal deadline changes nothing, outputs included. */
  relative = 12345;
  CHECK(!runtime_timer_slot_update(&slot, 0, 900, &relative));
  CHECK(!runtime_timer_slot_update(&slot, 0, 500, &relative));
  CHECK_EQ_U64(slot, 500);
  CHECK_EQ_U64(relative, 12345);

  /* An earlier one takes over. */
  CHECK(runtime_timer_slot_update(&slot, 10, 200, &relative));
  CHECK_EQ_U64(slot, 200);
  CHECK_EQ_U64(relative, 190);
}

/* Arm, fire (the notification clears the slot), rearm the remaining time:
 * the cycle a waiter repeats until its own deadline passes. */
static void test_repeated_fire_and_rearm(void) {
  uint64_t slot = 0;
  uint64_t now = 0;
  const uint64_t target = 1000;
  size_t arms = 0;
  while (now < target && arms < 64) {
    uint64_t relative = 0;
    if (runtime_timer_slot_update(&slot, now, target, &relative)) {
      arms++;
      CHECK_EQ_U64(now + relative, target);
    }
    /* A notification arrives partway to the deadline and clears the slot. */
    now += 300;
    slot = 0;
  }
  CHECK_EQ_U64(arms, 4);
}

int main(void) {
  test_empty_slot_arms();
  test_expired_deadline_fires_next_tick();
  test_nearest_deadline_owns_slot();
  test_repeated_fire_and_rearm();
  return check_finish("timer_slot");
}
