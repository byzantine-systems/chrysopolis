/* Root's spec consumer and the reference writer, without Microkit headers.
 * spec_writer.h is included twice to check its guard. */
#include "check.h"
#include "root_policy.h"
#include "spec.h"
#include "spec_writer.h"

#include <stdatomic.h>
#include <string.h>

static chryso_spec_page page;
static chryso_spec_page scratch;
static chryso_spec_page before;
static root_spec_state state;
static spec_writer writer;

/* Serial, timer, blk, eth and beam; the crasher (4) is absent. */
static constexpr uint64_t production = UINT64_C(0x2f);
static constexpr uint32_t hard_max = 64;

static uint32_t budget[chryso_child_count];
static uint8_t desired[chryso_child_count];

/* A policy that names every present child with value v, nothing else. */
static void policy_of(uint32_t v) {
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    const bool on = ((production >> c) & 1u) != 0;
    budget[c] = on ? v : 0;
    desired[c] = on ? chryso_root_desired_running : 0;
  }
}

static void fresh(void) {
  memset(&page, 0, sizeof(page));
  root_spec_init(&state);
  spec_writer_init(&writer);
}

static void commit(uint64_t generation, uint32_t v) {
  policy_of(v);
  CHECK(spec_writer_commit(&writer, &page, generation, budget, desired) ==
        spec_write_ok);
}

static root_spec_result consume(void) {
  return root_spec_consume(&state, &page, &scratch, production);
}

static uint8_t *bank_of(size_t bank) {
  return (uint8_t *)&page + sizeof(chryso_spec_header) +
         bank * sizeof(chryso_spec_bank);
}

static uint32_t active(void) { return atomic_load(&page.header.active_bank); }

/* Recomputes a bank's CRC after an edit, as a correct writer would. */
static void reseal(size_t bank) {
  uint8_t *at = bank_of(bank);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  crc = chryso_abi_crc32_update(crc, at, 12);
  crc = chryso_abi_crc32_zeros(crc, 4);
  crc = chryso_abi_crc32_update(crc, at + 16, 4);
  crc = chryso_abi_crc32_zeros(crc, 4);
  crc = chryso_abi_crc32_update(crc, at + 24, sizeof(chryso_spec_bank) - 24);
  crc = chryso_abi_crc32_final(crc);
  for (size_t i = 0; i < 4; i++) {
    at[offsetof(chryso_spec_bank, crc32) + i] = (uint8_t)(crc >> (8 * i));
  }
}

static bool same_policy(const root_spec_policy *a, const root_spec_policy *b) {
  if (a->generation != b->generation || a->bank != b->bank ||
      a->crc32 != b->crc32) {
    return false;
  }
  for (size_t c = 0; c < chryso_child_count; c++) {
    if (a->budget[c] != b->budget[c] || a->desired[c] != b->desired[c]) {
      return false;
    }
  }
  return true;
}

/* Consumes and checks the transactional contract: retained changes only on
 * applied, and then to a newer generation the page holds sealed. */
static root_spec_result consume_checked(void) {
  const root_spec_policy prior = state.retained;
  const root_spec_result result = consume();
  CHECK(result.outcome <= root_spec_kept);
  if (result.outcome == root_spec_applied) {
    CHECK(state.retained.generation > prior.generation);
    CHECK(state.retained.bank <= 1);
    CHECK(chryso_check_spec_bank((const uint8_t *)&page, sizeof(page),
                                 state.retained.bank) == chryso_abi_reject_ok);
    for (unsigned int c = 0; c < chryso_child_count; c++) {
      if (((production >> c) & 1u) == 0) {
        CHECK_EQ_U64(state.retained.budget[c], 0);
        CHECK_EQ_U64(state.retained.desired[c], 0);
      }
    }
  } else {
    CHECK(same_policy(&state.retained, &prior));
  }
  if (result.record) {
    CHECK(result.reason != chryso_root_spec_reject_unset);
  }
  return result;
}

/* --- the writer machine --- */

static void writer_to(size_t steps) {
  fresh();
  policy_of(3);
  CHECK(spec_writer_begin(&writer, &page, 1, budget, desired) == spec_write_ok);
  for (size_t i = 0; i < steps; i++) {
    CHECK(spec_writer_step(&writer, (spec_writer_step_kind)i) == spec_write_ok);
  }
}

static void test_writer_transitions(void) {
  /* Exactly one step is allowed from each phase, in protocol order, and a
   * refused step stores nothing. */
  for (size_t done = 0; done <= spec_writer_step_count; done++) {
    for (size_t step = 0; step <= spec_writer_step_count; step++) {
      writer_to(done);
      const spec_writer_phase phase = writer.phase;
      memcpy(&before, &page, sizeof(page));
      const spec_write_result result =
          spec_writer_step(&writer, (spec_writer_step_kind)step);
      if (step == done && step < spec_writer_step_count) {
        CHECK(result == spec_write_ok);
        CHECK(writer.phase != phase);
      } else {
        CHECK(result == spec_write_bad_state);
        CHECK(writer.phase == phase);
        CHECK(memcmp(&before, &page, sizeof(page)) == 0);
      }
    }
  }
  writer_to(spec_writer_step_count);
  CHECK(writer.phase == spec_writer_phase_flipped);
  CHECK(chryso_check_spec_header((const uint8_t *)&page, sizeof(page)) ==
        chryso_abi_reject_ok);
  CHECK(chryso_check_spec_bank((const uint8_t *)&page, sizeof(page), 1) ==
        chryso_abi_reject_ok);
  CHECK_EQ_U64(active(), 1);

  /* begin only from idle or flipped; an uninitialized writer cannot step. */
  for (size_t done = 1; done < spec_writer_step_count; done++) {
    writer_to(done);
    memcpy(&before, &page, sizeof(page));
    CHECK(spec_writer_begin(&writer, &page, 2, budget, desired) ==
          spec_write_bad_state);
    CHECK(memcmp(&before, &page, sizeof(page)) == 0);
  }
  spec_writer_init(&writer);
  CHECK(spec_writer_step(&writer, spec_writer_step_open) ==
        spec_write_bad_state);
}

static void test_writer_refusals(void) {
  fresh();
  policy_of(1);
  page.header.magic = 1; /* neither zero nor valid */
  memcpy(&before, &page, sizeof(page));
  CHECK(spec_writer_begin(&writer, &page, 1, budget, desired) ==
        spec_write_bad_page);
  CHECK(memcmp(&before, &page, sizeof(page)) == 0);
  CHECK(spec_writer_begin(&writer, nullptr, 1, budget, desired) ==
        spec_write_bad_page);

  /* The writer refuses a bank that cannot take another odd/even pair. */
  const struct {
    uint32_t seq;
    spec_write_result want;
  } edges[] = {
      {0xffff'fffcu, spec_write_ok},
      {0xffff'fffdu, spec_write_exhausted},
      {0xffff'fffeu, spec_write_exhausted},
      {0xffff'ffffu, spec_write_exhausted},
  };
  for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
    fresh();
    commit(1, 2); /* active is bank 1; the next target is bank 0 */
    atomic_store(&page.banks[0].bank_seq, edges[i].seq);
    memcpy(&before, &page, sizeof(page));
    CHECK(spec_writer_commit(&writer, &page, 2, budget, desired) ==
          edges[i].want);
    if (edges[i].want != spec_write_ok) {
      CHECK(memcmp(&before, &page, sizeof(page)) == 0);
    } else {
      CHECK_EQ_U64(atomic_load(&page.banks[0].bank_seq), 0xffff'fffeu);
    }
  }
}

/* --- the consumer --- */

static void test_absent(void) {
  fresh();
  CHECK(consume_checked().outcome == root_spec_absent);
  CHECK(root_spec_consume(&state, nullptr, &scratch, production).outcome ==
        root_spec_absent);
  CHECK(root_spec_consume(nullptr, &page, &scratch, production).outcome ==
        root_spec_absent);
  CHECK(root_spec_consume(&state, &page, nullptr, production).outcome ==
        root_spec_absent);

  /* A header with no published bank is still no spec, and not an error. */
  policy_of(1);
  CHECK(spec_writer_begin(&writer, &page, 1, budget, desired) == spec_write_ok);
  CHECK(spec_writer_step(&writer, spec_writer_step_open) == spec_write_ok);
  const root_spec_result r = consume_checked();
  CHECK(r.outcome == root_spec_absent && !r.record);

  /* A sealed generation 0 is an explicit "no spec". */
  fresh();
  commit(0, 5);
  CHECK(consume_checked().outcome == root_spec_absent);
  CHECK_EQ_U64(state.retained.generation, 0);
}

static void test_apply_and_unchanged(void) {
  fresh();
  commit(1, 2);
  root_spec_result r = consume_checked();
  CHECK(r.outcome == root_spec_applied && !r.record);
  CHECK_EQ_U64(state.retained.generation, 1);
  CHECK_EQ_U64(state.retained.bank, 1);
  r = consume_checked();
  CHECK(r.outcome == root_spec_unchanged && !r.record);

  commit(2, 3);
  r = consume_checked();
  CHECK(r.outcome == root_spec_applied);
  CHECK_EQ_U64(state.retained.generation, 2);
  CHECK_EQ_U64(state.retained.bank, 0);
  CHECK_EQ_U64(state.retained.budget[5], 3);
}

/* A first commit sealed but not flipped is a valid fallback. */
static void test_unflipped_fallback(void) {
  fresh();
  policy_of(4);
  CHECK(spec_writer_begin(&writer, &page, 1, budget, desired) == spec_write_ok);
  for (size_t i = 0; i < 4; i++) {
    CHECK(spec_writer_step(&writer, (spec_writer_step_kind)i) == spec_write_ok);
  }
  CHECK_EQ_U64(active(), 0);
  const root_spec_result r = consume_checked();
  CHECK(r.outcome == root_spec_applied && !r.record);
  CHECK_EQ_U64(state.retained.bank, 1);
}

typedef void (*page_edit)(void);

static void edit_header(void) { page.header.reserved[0] = 1; }
static void edit_torn(void) {
  atomic_store(&page.banks[active()].bank_seq, 5u);
}
static void edit_checksum(void) { bank_of(active())[30] ^= 1; }
static void edit_malformed(void) {
  bank_of(active())[offsetof(chryso_spec_bank, reserved_tail)] = 1;
  reseal(active());
}
static void edit_undeclared(void) {
  bank_of(active())[offsetof(chryso_spec_bank, budget) + 4 * 4] = 1;
  reseal(active());
}

/* gen1 then gen2 committed and applied, then the active bank is damaged:
 * Root reports why once, keeps gen2 (the fallback holds older gen1), and
 * reports again only after the page recovers and breaks again. */
static void test_rejections(void) {
  const struct {
    page_edit edit;
    chryso_root_spec_reject reason;
  } cases[] = {
      {edit_header, chryso_root_spec_reject_header},
      {edit_torn, chryso_root_spec_reject_bank_torn},
      {edit_checksum, chryso_root_spec_reject_bank_checksum},
      {edit_malformed, chryso_root_spec_reject_bank_malformed},
      {edit_undeclared, chryso_root_spec_reject_undeclared_child},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    fresh();
    commit(1, 2);
    CHECK(consume_checked().outcome == root_spec_applied);
    commit(2, 3);
    CHECK(consume_checked().outcome == root_spec_applied);
    cases[i].edit();
    root_spec_result r = consume_checked();
    CHECK(r.outcome == root_spec_kept);
    CHECK(r.record);
    CHECK(r.reason == cases[i].reason);
    CHECK_EQ_U64(state.retained.generation, 2);
    r = consume_checked();
    CHECK(!r.record && r.reason == cases[i].reason);
  }
}

static void test_regression_and_conflict(void) {
  /* A regressed active bank falls back to the identical prior bank. */
  fresh();
  commit(5, 2);
  CHECK(consume_checked().outcome == root_spec_applied);
  commit(4, 9);
  root_spec_result r = consume_checked();
  CHECK(r.outcome == root_spec_unchanged);
  CHECK(r.record && r.reason == chryso_root_spec_reject_regressed);
  CHECK_EQ_U64(r.generation, 4);
  CHECK_EQ_U64(state.retained.budget[0], 2);

  /* An equal generation with other content is a conflict, never applied. */
  fresh();
  commit(5, 2);
  CHECK(consume_checked().outcome == root_spec_applied);
  commit(5, 64);
  r = consume_checked();
  CHECK(r.outcome == root_spec_unchanged);
  CHECK(r.record && r.reason == chryso_root_spec_reject_conflict);
  CHECK_EQ_U64(r.bank, active());
  CHECK_EQ_U64(state.retained.budget[5], 2);

  /* An equal generation with equal content is the same policy. */
  fresh();
  commit(5, 2);
  CHECK(consume_checked().outcome == root_spec_applied);
  commit(5, 2);
  r = consume_checked();
  CHECK(r.outcome == root_spec_unchanged && !r.record);

  /* Both banks unusable: the retained policy stays and one event is due. */
  fresh();
  commit(5, 2);
  CHECK(consume_checked().outcome == root_spec_applied);
  commit(5, 7);
  bank_of(1 - active())[30] ^= 1;
  r = consume_checked();
  CHECK(r.outcome == root_spec_kept);
  CHECK(r.record && r.reason == chryso_root_spec_reject_conflict);
}

static void test_dedupe_resets(void) {
  fresh();
  commit(1, 2);
  CHECK(consume_checked().outcome == root_spec_applied);
  page.header.reserved[0] = 1;
  CHECK(consume_checked().record);
  CHECK(!consume_checked().record);
  page.header.reserved[0] = 0;
  CHECK(!consume_checked().record);
  page.header.reserved[0] = 1;
  CHECK(consume_checked().record);
}

static void test_clamps(void) {
  fresh();
  CHECK_EQ_U64(root_spec_budget(&state, 0, 8, hard_max), 8);
  policy_of(0);
  budget[1] = 1;
  budget[2] = 64;
  budget[3] = 65;
  budget[5] = UINT32_MAX;
  desired[0] = chryso_root_desired_stopped;
  desired[1] = 9;
  CHECK(spec_writer_commit(&writer, &page, 1, budget, desired) ==
        spec_write_ok);
  CHECK(consume_checked().outcome == root_spec_applied);
  CHECK_EQ_U64(root_spec_budget(&state, 0, 8, hard_max), 8);
  CHECK_EQ_U64(root_spec_budget(&state, 1, 8, hard_max), 1);
  CHECK_EQ_U64(root_spec_budget(&state, 2, 8, hard_max), 64);
  CHECK_EQ_U64(root_spec_budget(&state, 3, 8, hard_max), 64);
  CHECK_EQ_U64(root_spec_budget(&state, 5, 64, hard_max), 64);
  CHECK_EQ_U64(root_spec_budget(&state, 62, 8, hard_max), 8);
  /* No slots yet: every child is reported running. */
  CHECK(root_spec_desired(&state, 0, 0) == chryso_root_desired_running);
  CHECK(root_spec_desired(&state, 0, 1) == chryso_root_desired_stopped);
  CHECK(root_spec_desired(&state, 1, 2) == chryso_root_desired_running);
  CHECK(root_spec_desired(&state, 62, ~UINT64_C(0)) ==
        chryso_root_desired_running);
}

static void test_names(void) {
  for (uint32_t v = 0; v <= chryso_root_spec_reject_unstable_copy; v++) {
    CHECK(strcmp(root_spec_reject_name((chryso_root_spec_reject)v),
                 "invalid") != 0);
  }
  CHECK(strcmp(root_spec_reject_name((chryso_root_spec_reject)99), "invalid") ==
        0);
}

/* With no spec, and with a spec whose budgets are all zero, every decision
 * equals the legacy one. */
static void test_no_spec_parity(void) {
  for (int variant = 0; variant < 2; variant++) {
    fresh();
    if (variant == 1) {
      commit(1, 0);
      CHECK(consume_checked().outcome == root_spec_applied);
    }
    for (unsigned int c = 0; c < chryso_child_count; c++) {
      if (((production >> c) & 1u) == 0) {
        continue;
      }
      const uint32_t compiled = c == 5 ? 64 : 8;
      const uint32_t effective =
          root_spec_budget(&state, c, compiled, hard_max);
      CHECK_EQ_U64(effective, compiled);
      for (unsigned int count = 0; count <= 70; count++) {
        for (int gone = 0; gone < 2; gone++) {
          for (uint64_t entry = 0; entry <= 0x200000; entry += 0x200000) {
            const root_child_record record = {
                .lifetime_count = count,
                .state = gone ? root_child_gone : root_child_live,
            };
            CHECK(root_restart_decide(&record, effective, entry) ==
                  root_restart_decide(&record, compiled, entry));
          }
        }
      }
    }
  }
}

/* --- publication phases --- */

static uint64_t next(uint64_t *seed) {
  *seed ^= *seed << 13;
  *seed ^= *seed >> 7;
  *seed ^= *seed << 17;
  return *seed;
}

/* Generation k is committed one step at a time over k - 1 complete commits.
 * Root either consumed every earlier commit (current) or none (cold). Only a
 * flip publishes k as preferred; a sealed unflipped bank is used only when
 * the active bank has nothing. */
static void test_publication_phases(void) {
  for (uint64_t k = 1; k <= 3; k++) {
    for (size_t done = 0; done <= spec_writer_step_count; done++) {
      for (int current = 0; current < 2; current++) {
        fresh();
        for (uint64_t g = 1; g < k; g++) {
          commit(g, (uint32_t)g);
          if (current) {
            CHECK(consume_checked().outcome == root_spec_applied);
          }
        }
        policy_of((uint32_t)k);
        CHECK(spec_writer_begin(&writer, &page, k, budget, desired) ==
              spec_write_ok);
        for (size_t i = 0; i < done; i++) {
          CHECK(spec_writer_step(&writer, (spec_writer_step_kind)i) ==
                spec_write_ok);
        }
        const root_spec_result r = consume_checked();
        uint64_t want = k - 1;
        if (done == spec_writer_step_count || (k == 1 && done == 4)) {
          want = k;
        }
        CHECK_EQ_U64(state.retained.generation, want);
        CHECK(!r.record);
        /* One complete prior bank always validates during the write. */
        if (k > 1) {
          CHECK(chryso_check_spec_bank((const uint8_t *)&page, sizeof(page),
                                       done == spec_writer_step_count
                                           ? 1 - active()
                                           : active()) == chryso_abi_reject_ok);
        }
      }
    }
  }

  /* Any subset of the open bank's payload bytes landing never applies it. */
  uint64_t seed = UINT64_C(0x3c6ef372fe94f82b);
  for (int round = 0; round < 400; round++) {
    fresh();
    commit(1, 1);
    CHECK(consume_checked().outcome == root_spec_applied);
    policy_of(9);
    CHECK(spec_writer_begin(&writer, &page, 2, budget, desired) ==
          spec_write_ok);
    memcpy(&before, &page, sizeof(page));
    for (size_t i = 0; i < 3; i++) { /* open, write, seal */
      CHECK(spec_writer_step(&writer, (spec_writer_step_kind)i) ==
            spec_write_ok);
    }
    /* Keep the odd sequence; roll back a random subset of other bytes. */
    uint8_t *now = bank_of(0);
    const uint8_t *was = (const uint8_t *)&before + sizeof(chryso_spec_header);
    for (size_t b = 0; b < sizeof(chryso_spec_bank); b++) {
      const bool seq = b >= offsetof(chryso_spec_bank, bank_seq) &&
                       b < offsetof(chryso_spec_bank, bank_seq) + 4;
      if (!seq && (next(&seed) & 1u) != 0) {
        now[b] = was[b];
      }
    }
    /* Half the rounds also break the active bank, so the torn one is read
     * as the only fallback. */
    const bool fallback = (round & 1) != 0;
    if (fallback) {
      bank_of(1)[30] ^= 1;
    }
    const root_spec_result r = consume_checked();
    if (state.retained.generation != 1) {
      fprintf(stderr, "partial payload applied, round %d\n", round);
    }
    CHECK(r.outcome == (fallback ? root_spec_kept : root_spec_unchanged));
    CHECK_EQ_U64(state.retained.generation, 1);
  }
}

/* --- fuzzed pages --- */

static void test_fuzz(void) {
  const uint64_t start_seed = UINT64_C(0xa54ff53a5f1d36f1);
  uint64_t seed = start_seed;
  for (int round = 0; round < 3000; round++) {
    const int shape = round % 3;
    fresh();
    if (shape == 0) {
      uint8_t *bytes = (uint8_t *)&page;
      for (size_t i = 0; i < sizeof(page); i++) {
        bytes[i] = (uint8_t)next(&seed);
      }
    } else {
      commit(3, 2);
      commit(4, 5);
      const int flips = 1 + (int)(next(&seed) % 4);
      for (int f = 0; f < flips; f++) {
        const size_t at = (size_t)(next(&seed) % sizeof(page));
        ((uint8_t *)&page)[at] ^= (uint8_t)(1u << (next(&seed) % 8));
      }
      if (shape == 2) {
        reseal(0);
        reseal(1);
      }
    }
    if ((next(&seed) & 1u) != 0) {
      state.retained.generation = next(&seed) % 6;
    }
    const root_spec_state at_start = state;
    const root_spec_result first = consume_checked();
    /* The same page from the same state gives the same answer. */
    root_spec_state replay = at_start;
    const root_spec_result again =
        root_spec_consume(&replay, &page, &scratch, production);
    CHECK(again.outcome == first.outcome && again.reason == first.reason &&
          again.record == first.record);
    CHECK(same_policy(&replay.retained, &state.retained));
    /* A repeat read never reports twice or applies twice. */
    const root_spec_result repeat = consume_checked();
    CHECK(!repeat.record);
    CHECK(repeat.outcome != root_spec_applied);
    for (unsigned int c = 0; c < chryso_child_count; c++) {
      const uint32_t b = root_spec_budget(&state, c, 8, hard_max);
      CHECK(b >= 1 && b <= hard_max);
    }
    if (check_failed != 0) {
      fprintf(stderr, "fuzz start seed 0x%016" PRIx64 " round %d\n", start_seed,
              round);
      return;
    }
  }
}

int main(void) {
  test_writer_transitions();
  test_writer_refusals();
  test_absent();
  test_apply_and_unchanged();
  test_unflipped_fallback();
  test_rejections();
  test_regression_and_conflict();
  test_dedupe_resets();
  test_clamps();
  test_names();
  test_no_spec_parity();
  test_publication_phases();
  test_fuzz();
  return check_finish("root_spec");
}
