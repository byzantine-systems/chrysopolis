/* Root's PPC command decisions without Microkit headers. A decision with no
 * action is what keeps the adapter away from every capability, so each
 * rejection below also checks action == root_command_none. command.h is
 * included twice to check its guard. */
#include "check.h"
#include "command.h"

static constexpr unsigned int pp = 11;
static constexpr uint32_t capacity = 32;
/* 32 tokens per 3200 ticks: one token every 100 ticks. */
static constexpr uint64_t refill = 3200;
static constexpr uint64_t interval = 100;
/* Serial, timer, blk, eth, crasher and beam present; drivers 0..3. */
static constexpr uint64_t present = UINT64_C(0x3f);
static constexpr uint64_t drivers = UINT64_C(0x0f);
static constexpr unsigned int beam = 5;
static constexpr unsigned int crasher = 4;
static constexpr unsigned int timer = 1;

static root_child_record records[chryso_child_count];
static root_command_state state;
static root_command_view view;

static void fresh(void) {
  for (size_t i = 0; i < chryso_child_count; i++) {
    records[i] = (root_child_record){};
  }
  root_command_init(&state, capacity, refill, 1000, true);
  view = (root_command_view){
      .abi_version = chryso_abi_version,
      .root_generation = 1,
      .beam_incarnation = 1,
      .cntfrq = 62500000,
      .seq = 2,
      .publishing = true,
      .present_mask = present,
      .driver_mask = drivers,
      .child_count = chryso_child_count,
      .records = records,
  };
}

static root_command_decision call(uint16_t opcode, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, uint64_t now) {
  const uint64_t args[root_command_words] = {a0, a1, a2, a3};
  return root_command_decide(&state, &view, opcode, args, now, true);
}

static root_command_decision restart(unsigned int child, uint64_t now) {
  return call(chryso_pp_opcode_restart_child, child, view.root_generation,
              view.seq, records[child].lifetime_count, now);
}

/* What the adapter does after an ok restart: charge, publish, reply. */
static void perform(root_command_decision *d) {
  CHECK(d->action == root_command_restart);
  records[d->child.value].lifetime_count++;
  view.seq += 2;
  d->values[0] = chryso_root_event_kind_restart;
  d->values[1] = 0;
  d->values[2] = records[d->child.value].lifetime_count;
  d->values[3] = view.seq;
  root_command_complete(&state, d);
}

static void rejected_without_action(root_command_decision d,
                                    chryso_ctl_result want) {
  CHECK_EQ_U64(d.result, want);
  CHECK(d.action == root_command_none);
}

static void test_frame(void) {
  fresh();
  CHECK_EQ_U64(root_command_frame(&state, pp, pp, chryso_pp_opcode_hello, 4),
               chryso_ctl_result_ok);
  CHECK_EQ_U64(
      root_command_frame(&state, pp + 1, pp, chryso_pp_opcode_hello, 4),
      chryso_ctl_result_refused);
  CHECK_EQ_U64(root_command_frame(&state, 0, pp, chryso_pp_opcode_hello, 4),
               chryso_ctl_result_refused);
  const uint64_t counts[] = {0, 1, 3, 5, 64, UINT64_MAX};
  for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    CHECK_EQ_U64(root_command_frame(&state, pp, pp,
                                    chryso_pp_opcode_restart_child, counts[i]),
                 chryso_ctl_result_bad_arg);
  }
  const uint64_t labels[] = {0, 8, 0xffff, 0x10001, 0x10007, UINT64_MAX};
  for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
    CHECK_EQ_U64(root_command_frame(&state, pp, pp, labels[i], 4),
                 chryso_ctl_result_bad_arg);
  }
  for (uint64_t op = chryso_pp_opcode_hello; op <= chryso_pp_opcode_sync;
       op++) {
    CHECK_EQ_U64(root_command_frame(&state, pp, pp, op, 4),
                 chryso_ctl_result_ok);
  }
  /* Frame rejections are tallied but spend no token. */
  CHECK_EQ_U64(state.rejected, 2 + 6 + 6);
  CHECK_EQ_U64(state.tokens, capacity);
}

static void test_unused_words(void) {
  fresh();
  rejected_without_action(
      call(chryso_pp_opcode_hello, chryso_abi_version, 1, 0, 0, 1000),
      chryso_ctl_result_bad_arg);
  rejected_without_action(
      call(chryso_pp_opcode_hello, chryso_abi_version, 0, 0, 1, 1000),
      chryso_ctl_result_bad_arg);
  for (size_t w = 0; w < root_command_words; w++) {
    uint64_t args[root_command_words] = {};
    args[w] = 1;
    rejected_without_action(root_command_decide(&state, &view,
                                                chryso_pp_opcode_sync, args,
                                                1000, true),
                            chryso_ctl_result_bad_arg);
  }
  CHECK_EQ_U64(state.tokens, capacity);
  CHECK_EQ_U64(state.rejected, 6);
}

static void test_hello(void) {
  fresh();
  root_command_decision d =
      call(chryso_pp_opcode_hello, chryso_abi_version, 0, 0, 0, 1000);
  CHECK_EQ_U64(d.result, chryso_ctl_result_ok);
  CHECK(d.action == root_command_none);
  CHECK_EQ_U64(d.values[0], chryso_abi_version);
  CHECK_EQ_U64(d.values[1], 1);
  CHECK_EQ_U64(d.values[2], 1);
  CHECK_EQ_U64(d.values[3], 62500000);
  d = call(chryso_pp_opcode_hello, chryso_abi_version + 1, 0, 0, 0, 1000);
  rejected_without_action(d, chryso_ctl_result_bad_version);
  CHECK_EQ_U64(d.values[0], chryso_abi_version);
  /* HELLO does not depend on publication. */
  view.publishing = false;
  CHECK_EQ_U64(
      call(chryso_pp_opcode_hello, chryso_abi_version, 0, 0, 0, 1000).result,
      chryso_ctl_result_ok);
  CHECK_EQ_U64(state.tokens, capacity - 3);
}

static void test_sync(void) {
  fresh();
  (void)root_command_frame(&state, pp + 1, pp, chryso_pp_opcode_sync, 4);
  (void)call(chryso_pp_opcode_hello, chryso_abi_version, 0, 0, 0, 1000);
  root_command_decision d = call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000);
  CHECK_EQ_U64(d.result, chryso_ctl_result_ok);
  CHECK_EQ_U64(d.values[0], 2);
  CHECK_EQ_U64(d.values[1], 2);
  CHECK_EQ_U64(d.values[2], 0);
  CHECK_EQ_U64(d.values[3], 1);
  view.publishing = false;
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000),
                          chryso_ctl_result_refused);
}

static void test_restart_ok_and_duplicate(void) {
  fresh();
  root_command_decision d = restart(timer, 1000);
  CHECK_EQ_U64(d.result, chryso_ctl_result_ok);
  CHECK_EQ_U64(d.child.value, timer);
  const uint64_t observed_seq = view.seq;
  perform(&d);
  /* The lost-reply retry: identical words, now behind the published seq. */
  const root_command_decision again =
      call(chryso_pp_opcode_restart_child, timer, 1, observed_seq, 0, 1000);
  rejected_without_action(again, chryso_ctl_result_already_done);
  for (size_t i = 0; i < root_command_words; i++) {
    CHECK_EQ_U64(again.values[i], d.values[i]);
  }
  CHECK_EQ_U64(records[timer].lifetime_count, 1);
  /* A fresh observation is a new command. */
  d = restart(timer, 1000);
  CHECK_EQ_U64(d.result, chryso_ctl_result_ok);
  perform(&d);
  /* The first command again, now reordered behind the second: stale. */
  const root_command_decision late =
      call(chryso_pp_opcode_restart_child, timer, 1, observed_seq, 0, 1000);
  rejected_without_action(late, chryso_ctl_result_stale);
  CHECK_EQ_U64(late.values[0], 1);
  CHECK_EQ_U64(late.values[1], view.seq);
  CHECK_EQ_U64(late.values[2], 2);
}

static void test_restart_preconditions(void) {
  fresh();
  rejected_without_action(
      call(chryso_pp_opcode_restart_child, timer, 2, view.seq, 0, 1000),
      chryso_ctl_result_stale);
  rejected_without_action(
      call(chryso_pp_opcode_restart_child, timer, 1, view.seq + 2, 0, 1000),
      chryso_ctl_result_stale);
  rejected_without_action(
      call(chryso_pp_opcode_restart_child, timer, 1, view.seq, 1, 1000),
      chryso_ctl_result_stale);
  rejected_without_action(restart(beam, 1000), chryso_ctl_result_refused);
  rejected_without_action(restart(crasher, 1000), chryso_ctl_result_refused);
  const uint64_t bad_children[] = {6,          chryso_child_count, 64,
                                   UINT32_MAX, UINT64_C(1) << 32,  UINT64_MAX};
  for (size_t i = 0; i < sizeof(bad_children) / sizeof(bad_children[0]); i++) {
    rejected_without_action(call(chryso_pp_opcode_restart_child,
                                 bad_children[i], 1, view.seq, 0, 1000),
                            chryso_ctl_result_bad_arg);
  }
  /* The crasher is absent in production. */
  view.present_mask = present & ~(UINT64_C(1) << crasher);
  rejected_without_action(restart(crasher, 1000), chryso_ctl_result_bad_arg);
  records[timer].state = root_child_gone;
  rejected_without_action(restart(timer, 1000), chryso_ctl_result_refused);
  records[timer].state = root_child_live;
  view.publishing = false;
  rejected_without_action(restart(timer, 1000), chryso_ctl_result_refused);
}

/* A completed command stays already_done even if publication stopped
 * during it or the child went gone afterwards. */
static void test_duplicate_survives_later_state(void) {
  fresh();
  root_command_decision d = restart(timer, 1000);
  const uint64_t observed_seq = view.seq;
  perform(&d);
  view.publishing = false;
  records[timer].state = root_child_gone;
  rejected_without_action(
      call(chryso_pp_opcode_restart_child, timer, 1, observed_seq, 0, 1000),
      chryso_ctl_result_already_done);
}

static void test_slot_opcodes(void) {
  fresh();
  const uint16_t slots[] = {
      chryso_pp_opcode_start_slot, chryso_pp_opcode_stop_child,
      chryso_pp_opcode_resume_child, chryso_pp_opcode_reclaim_slot};
  /* One interval per call keeps the bucket from limiting the sweep. */
  uint64_t now = 1000;
  for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
    for (uint64_t child = 0; child < chryso_child_count; child++) {
      now += interval;
      rejected_without_action(call(slots[i], child, 1, view.seq, 0, now),
                              chryso_ctl_result_refused);
    }
  }
}

static void test_bucket_refill(void) {
  fresh();
  for (uint32_t i = 0; i < capacity; i++) {
    CHECK_EQ_U64(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000).result,
                 chryso_ctl_result_ok);
  }
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000),
                          chryso_ctl_result_rate_limited);
  /* One tick short of an interval: still empty. */
  rejected_without_action(
      call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000 + interval - 1),
      chryso_ctl_result_rate_limited);
  CHECK_EQ_U64(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000 + interval).result,
               chryso_ctl_result_ok);
  rejected_without_action(
      call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000 + interval),
      chryso_ctl_result_rate_limited);
  /* 2.5 intervals later: two tokens, the half interval carries over. */
  const uint64_t t = 1000 + interval + 2 * interval + interval / 2;
  CHECK_EQ_U64(call(chryso_pp_opcode_sync, 0, 0, 0, 0, t).result,
               chryso_ctl_result_ok);
  CHECK_EQ_U64(state.tokens, 1);
  CHECK_EQ_U64(call(chryso_pp_opcode_sync, 0, 0, 0, 0, t + interval / 2).result,
               chryso_ctl_result_ok);
  CHECK_EQ_U64(state.tokens, 1);
  /* A long idle period caps at capacity. */
  (void)call(chryso_pp_opcode_sync, 0, 0, 0, 0, UINT64_MAX / 2);
  CHECK_EQ_U64(state.tokens, capacity - 1);
  CHECK_EQ_U64(state.last_ticks, UINT64_MAX / 2);
  CHECK_EQ_U64(state.rate_limited, 3);
  CHECK(state.refill == root_command_refill_running);
}

static void test_clock_rollback(void) {
  fresh();
  for (uint32_t i = 0; i < capacity; i++) {
    (void)call(chryso_pp_opcode_sync, 0, 0, 0, 0, 5000);
  }
  CHECK_EQ_U64(state.tokens, 0);
  /* The sample at 5000 granted nothing beyond the full bucket; go back. */
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 999),
                          chryso_ctl_result_rate_limited);
  CHECK(state.refill == root_command_refill_off);
  /* off is absorbing: no later sample refills. */
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, UINT64_MAX),
                          chryso_ctl_result_rate_limited);
  CHECK_EQ_U64(state.tokens, 0);
}

static void test_clock_unavailable(void) {
  fresh();
  const uint64_t args[root_command_words] = {};
  CHECK_EQ_U64(root_command_decide(&state, &view, chryso_pp_opcode_sync, args,
                                   2000, false)
                   .result,
               chryso_ctl_result_ok);
  CHECK(state.refill == root_command_refill_off);
  CHECK_EQ_U64(state.tokens, capacity - 1);
  /* Unavailable at init: spend the initial bucket, never refill. */
  root_command_init(&state, capacity, refill, 0, false);
  CHECK(state.refill == root_command_refill_off);
  for (uint32_t i = 0; i < capacity; i++) {
    CHECK_EQ_U64(call(chryso_pp_opcode_sync, 0, 0, 0, 0, i * interval).result,
                 chryso_ctl_result_ok);
  }
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, UINT64_MAX),
                          chryso_ctl_result_rate_limited);
  /* No refill interval or no capacity. */
  root_command_init(&state, capacity, 0, 0, true);
  CHECK(state.refill == root_command_refill_off);
  root_command_init(&state, 0, refill, 0, true);
  rejected_without_action(call(chryso_pp_opcode_sync, 0, 0, 0, 0, 1000),
                          chryso_ctl_result_rate_limited);
  /* An interval that does not divide evenly rounds up. */
  root_command_init(&state, 3, 10, 0, true);
  CHECK_EQ_U64(state.interval_ticks, 4);
}

/* A rate-limited, stale or refused restart must not consume the fence. */
static void test_rejections_keep_fence(void) {
  fresh();
  root_command_decision d = restart(timer, 1000);
  const uint64_t observed_seq = view.seq;
  perform(&d);
  (void)restart(beam, 1000);
  (void)call(chryso_pp_opcode_restart_child, timer, 1, 0, 0, 1000);
  CHECK(state.fence == root_command_fence_completed);
  CHECK_EQ_U64(
      call(chryso_pp_opcode_restart_child, timer, 1, observed_seq, 0, 1000)
          .result,
      chryso_ctl_result_already_done);
}

/* xorshift64: deterministic, no libc rand. */
static uint64_t next(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return *s;
}

/* Random command streams, including verbatim replays of earlier commands:
 * no observed seq ever yields two restarts, and every action matched the
 * state it was decided against. */
static void test_random_streams(void) {
  for (uint64_t seed = 1; seed <= 64; seed++) {
    fresh();
    uint64_t s = seed * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t history[32][root_command_words] = {};
    size_t kept = 0;
    uint64_t acted_seqs[512] = {};
    size_t acted = 0;
    uint64_t now = 1000;
    for (size_t step = 0; step < 400; step++) {
      now += next(&s) % (2 * interval);
      uint64_t args[root_command_words] = {};
      const unsigned int child = (unsigned int)(next(&s) % 7);
      switch (next(&s) % 4) {
      case 0:
        if (kept != 0) {
          for (size_t w = 0; w < root_command_words; w++) {
            args[w] = history[next(&s) % kept][w];
          }
          break;
        }
        [[fallthrough]];
      case 1:
        args[0] = child;
        args[1] = 1;
        args[2] = view.seq;
        args[3] = records[child].lifetime_count;
        break;
      default:
        args[0] = child;
        args[1] = next(&s) % 3;
        args[2] = view.seq - 2 * (next(&s) % 3);
        args[3] = records[child].lifetime_count + next(&s) % 2;
        break;
      }
      if (next(&s) % 16 == 0) {
        view.seq += 2; /* an unrelated fault publication */
      }
      if (kept < 32) {
        for (size_t w = 0; w < root_command_words; w++) {
          history[kept][w] = args[w];
        }
        kept++;
      }
      root_command_decision d = root_command_decide(
          &state, &view, chryso_pp_opcode_restart_child, args, now, true);
      if (d.action == root_command_none) {
        continue;
      }
      CHECK(((drivers >> d.child.value) & 1u) != 0);
      CHECK_EQ_U64(args[1], view.root_generation);
      CHECK_EQ_U64(args[2], view.seq);
      CHECK_EQ_U64(args[3], records[d.child.value].lifetime_count);
      for (size_t i = 0; i < acted; i++) {
        CHECK(acted_seqs[i] != args[2]);
      }
      if (acted < sizeof(acted_seqs) / sizeof(acted_seqs[0])) {
        acted_seqs[acted++] = args[2];
      }
      perform(&d);
    }
    CHECK(acted > 0);
  }
}

int main(void) {
  test_frame();
  test_unused_words();
  test_hello();
  test_sync();
  test_restart_ok_and_duplicate();
  test_restart_preconditions();
  test_duplicate_survives_later_state();
  test_slot_opcodes();
  test_bucket_refill();
  test_clock_rollback();
  test_clock_unavailable();
  test_rejections_keep_fence();
  test_random_streams();
  return check_finish("root_command");
}
