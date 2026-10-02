/* Root's publication and copy contract, exercised without Microkit headers. */
#include "check.h"
#include "root_status_reader.h"
#include "status.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

static chryso_root_status_page page;
static chryso_root_status_page scratch;
static chryso_root_status_page output;
static chryso_root_status_page prior;
static chryso_root_status_page mutated;
static root_status_state state;

/* Serial, timer, blk, eth and beam; the crasher (4) is absent. */
static constexpr uint64_t production = UINT64_C(0x2f);

static bool page_valid(void) {
  return chryso_check_root_status_page((const uint8_t *)&page, sizeof(page)) ==
         chryso_abi_reject_ok;
}

static void start(uint64_t mask) {
  memset(&page, 0xa5, sizeof(page));
  root_status_init(&state, &page, mask, 100, 62500000, 8, 64, 5);
  CHECK_EQ_U64(atomic_load(&page.header.seq), 2);
  CHECK_EQ_U64(page.header.event_head, 1);
  CHECK(page.events[0].kind == chryso_root_event_kind_boot);
  CHECK(page.events[0].child == chryso_root_event_no_child);
  CHECK(page.children[4].state == ((mask & (UINT64_C(1) << 4)) != 0
                                       ? chryso_root_child_wire_state_live
                                       : chryso_root_child_wire_state_unset));
  CHECK_EQ_U64(page.children[5].effective_budget, 64);
  CHECK_EQ_U64(page.children[2].effective_budget, 8);
  CHECK_EQ_U64(page.children[40].effective_budget, 0);
  CHECK(page_valid());
}

static chryso_root_event fault_event(uint8_t child, uint16_t flags,
                                     uint64_t ticks) {
  return (chryso_root_event){.ticks = ticks,
                             .kind = chryso_root_event_kind_fault,
                             .child = child,
                             .flags = flags,
                             .detail = 6};
}

static void test_fault_and_giveup(void) {
  start(production);
  root_child_record child = {};
  root_status_note_fault(&state, 2, 6, 1, 0x40, 0, 110, true);
  root_record_charge(&child);
  root_status_note_restart(&state, 2, 114, true, false);
  chryso_root_event events[2] = {
      fault_event(2, chryso_fault_mr0_valid, 110),
      {.ticks = 114,
       .kind = chryso_root_event_kind_restart,
       .child = 2,
       .a = 1},
  };
  CHECK(root_status_publish(&state, &page, 2, &child, 8, 114, true, events, 2));
  CHECK_EQ_U64(page.header.seq, 4);
  CHECK_EQ_U64(page.header.event_head, 3);
  CHECK_EQ_U64(page.children[2].lifetime_count, 1);
  CHECK_EQ_U64(page.children[2].fault_mr0, 0x40);
  CHECK_EQ_U64(page.children[2].flags, chryso_fault_mr0_valid);
  CHECK_EQ_U64(page.children[2].cumulative_down_ticks, 4);
  CHECK_EQ_U64(page.children[2].last_restart_ticks, 114);
  CHECK_EQ_U64(page.children[2].boot_ticks, 100);
  CHECK(page_valid());

  root_status_note_fault(&state, 2, 6, 0, 0, 0, 120, true);
  root_record_give_up(&child);
  root_status_note_gone(&state, 2, 121, true);
  events[0] = fault_event(2, 0, 120);
  events[1] =
      (chryso_root_event){.ticks = 121,
                          .kind = chryso_root_event_kind_giveup,
                          .child = 2,
                          .detail = chryso_root_giveup_reason_budget_exhausted,
                          .a = 1};
  CHECK(root_status_publish(&state, &page, 2, &child, 8, 121, true, events, 2));
  CHECK_EQ_U64(page.children[2].state, chryso_root_child_wire_state_gone);
  CHECK_EQ_U64(page.children[2].flags, chryso_down_interval_open);
  CHECK_EQ_U64(page.children[2].cumulative_down_ticks, 4);
  CHECK(page_valid());
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_ok);
}

static void test_reader_bounds(void) {
  start(production | (UINT64_C(1) << 4));
  memset(&output, 0x7c, sizeof(output));
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 2) ==
        root_status_read_ok);
  CHECK_EQ_U64(output.header.event_head, 1);
  CHECK(memcmp(&output, &page, sizeof(page)) == 0);

  /* Odd, zero and no attempts are torn; output keeps the last good copy. */
  static constexpr uint64_t unpublished[] = {0, 3};
  for (size_t i = 0; i < 2; i++) {
    atomic_store(&page.header.seq, unpublished[i]);
    CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 2) ==
          root_status_read_torn);
    CHECK_EQ_U64(atomic_load(&output.header.seq), 2);
  }
  atomic_store(&page.header.seq, 2);
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 0) ==
        root_status_read_torn);
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output) - 1, 2) ==
        root_status_read_size);
  CHECK(root_status_read(&page, &page, &output, sizeof(output), 2) ==
        root_status_read_size);
  CHECK(root_status_read(&page, &scratch, &scratch, sizeof(output), 2) ==
        root_status_read_size);
  CHECK(root_status_read(nullptr, &scratch, &output, sizeof(output), 2) ==
        root_status_read_size);
}

/* The test acts as the writer between the two halves of one attempt. */
static void test_writer_between_halves(void) {
  start(production);
  const chryso_root_event event = {.kind = chryso_root_event_kind_control,
                                   .child = 1,
                                   .detail =
                                       chryso_root_control_kind_fault_inject};
  uint64_t seq = 0;
  CHECK(root_status_copy_begin(&page, &scratch, &seq));
  CHECK(
      root_status_publish(&state, &page, 1, nullptr, 0, 101, true, &event, 1));
  CHECK(root_status_copy_finish(&page, &scratch, &output, seq) ==
        root_status_read_torn);

  CHECK(root_status_copy_begin(&page, &scratch, &seq));
  atomic_store(&page.header.seq, seq + 1);
  CHECK(root_status_copy_finish(&page, &scratch, &output, seq) ==
        root_status_read_torn);
  atomic_store(&page.header.seq, seq);

  /* A scratch copy whose own seq disagrees with the bracket is not used. */
  CHECK(root_status_copy_begin(&page, &scratch, &seq));
  atomic_store(&scratch.header.seq, seq + 2);
  memset(&output, 0x7c, sizeof(output));
  CHECK(root_status_copy_finish(&page, &scratch, &output, seq) ==
        root_status_read_invalid);
  CHECK_EQ_U64(output.header.magic, UINT64_C(0x7c7c7c7c7c7c7c7c));
}

/* Every retry sees a different completed publication between its sequence
 * reads. The bounded reader is just this attempt repeated; none may publish a
 * mixed copy or alter the caller's last good output. */
static void test_repeated_sequence_churn(void) {
  start(production);
  memset(&output, 0x7c, sizeof(output));
  memset(&mutated, 0x7c, sizeof(mutated));
  chryso_root_event event = {
      .kind = chryso_root_event_kind_control,
      .child = 0,
      .detail = chryso_root_control_kind_fault_inject,
  };
  for (size_t attempt = 0; attempt < 8; attempt++) {
    uint64_t seq = 0;
    CHECK(root_status_copy_begin(&page, &scratch, &seq));
    event.ticks = 101 + attempt;
    CHECK(root_status_publish(&state, &page, 0, nullptr, 0, event.ticks, true,
                              &event, 1));
    CHECK(root_status_copy_finish(&page, &scratch, &output, seq) ==
          root_status_read_torn);
    CHECK(memcmp(&output, &mutated, sizeof(output)) == 0);
  }
}

static void test_semantic_rejections(void) {
  start(production);
  page.children[2].flags = chryso_fault_mr1_valid;
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_invalid);
  page.children[2].flags = 0;
  page.children[40].effective_budget = 1;
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_invalid);
  page.children[40].effective_budget = 0;
  page.events[0] =
      (chryso_root_event){.kind = chryso_root_event_kind_control,
                          .child = 40,
                          .detail = chryso_root_control_kind_fault_inject};
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_invalid);
  page.events[0] =
      (chryso_root_event){.kind = chryso_root_event_kind_giveup, .child = 2};
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_invalid);
  page.events[0].detail = chryso_root_giveup_reason_no_entry;
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_ok);
}

static void test_exhaustion_and_misuse(void) {
  start(production);
  const chryso_root_event event = {.kind = chryso_root_event_kind_control,
                                   .child = 0,
                                   .detail =
                                       chryso_root_control_kind_fault_inject};
  /* An absent child is misuse: publication stops, the page is untouched. */
  CHECK(!root_status_publish(&state, &page, 4, &(root_child_record){}, 8, 0,
                             false, &event, 1));
  CHECK(state.misused && !state.exhausted && !state.enabled);
  CHECK_EQ_U64(atomic_load(&page.header.seq), 2);

  start(production);
  const chryso_root_event three[3] = {event, event, event};
  CHECK(!root_status_publish(&state, &page, 0, nullptr, 0, 0, false, three, 3));
  CHECK(state.misused && !state.exhausted);

  start(production);
  state.seq = UINT64_MAX - 1;
  CHECK(
      !root_status_publish(&state, &page, 0, nullptr, 0, 0, false, &event, 1));
  CHECK(state.exhausted && !state.misused && !state.enabled);
  CHECK_EQ_U64(page.header.event_head, 1);
  CHECK_EQ_U64(atomic_load(&page.header.seq), 2);
  /* Disabled stays disabled, and the last snapshot still reads. */
  state.seq = 2;
  CHECK(
      !root_status_publish(&state, &page, 0, nullptr, 0, 0, false, &event, 1));
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 1) ==
        root_status_read_ok);
}

static void test_unmapped_and_clock(void) {
  root_status_init(&state, nullptr, production, 0, 0, 8, 64, 5);
  CHECK(!state.enabled);
  CHECK(!root_status_publish(&state, nullptr, 5, &(root_child_record){}, 64, 0,
                             false, nullptr, 0));
  CHECK(!state.exhausted && !state.misused);

  start(production);
  root_status_note_restart(&state, 5, 0, false, true);
  CHECK_EQ_U64(state.beam_incarnation, 2);
  const chryso_root_event event = {.kind = chryso_root_event_kind_restart,
                                   .child = 5};
  CHECK(root_status_publish(&state, &page, 5, &(root_child_record){}, 64, 0,
                            false, &event, 1));
  CHECK_EQ_U64(page.header.cntfrq, 0);
  CHECK_EQ_U64(page.header.now_ticks, 0);
  CHECK_EQ_U64(page.header.beam_incarnation, 2);
  root_status_freshness fresh = {};
  CHECK(root_status_observe(nullptr, &page, 1, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK(!fresh.age_known);
  state.beam_incarnation = UINT64_MAX;
  root_status_note_restart(&state, 5, 0, false, true);
  CHECK(!state.enabled && state.exhausted);
  CHECK_EQ_U64(page.header.beam_incarnation, 2);
}

static void test_observe(void) {
  start(production);
  memcpy(&prior, &page, sizeof(prior));
  root_status_freshness fresh = {};
  CHECK(root_status_observe(nullptr, &page, 125, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK(fresh.age_known && fresh.changed);
  CHECK_EQ_U64(fresh.age_ticks, 25);
  CHECK(root_status_observe(&prior, &page, 125, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK(!fresh.changed);
  /* A different counter frequency leaves age unknown. */
  CHECK(root_status_observe(&prior, &page, 125, 1000000, &fresh) ==
        root_status_observe_ok);
  CHECK(!fresh.age_known);

  /* Equal seq with different bytes. */
  page.children[2].lifetime_count = 1;
  CHECK(root_status_observe(&prior, &page, 125, 62500000, &fresh) ==
        root_status_observe_regressed);
  page.children[2].lifetime_count = 0;

  root_child_record record = {};
  root_record_charge(&record);
  CHECK(
      root_status_publish(&state, &page, 2, &record, 8, 130, true, nullptr, 0));
  CHECK(root_status_observe(&prior, &page, 130, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK(fresh.changed);
  /* Counters never go backwards within one generation. */
  CHECK(root_status_observe(&page, &prior, 130, 62500000, &fresh) ==
        root_status_observe_regressed);
  page.header.beam_incarnation = 0;
  CHECK(root_status_observe(&prior, &page, 130, 62500000, &fresh) ==
        root_status_observe_regressed);
  page.header.beam_incarnation = 1;
  page.header.root_generation = 0;
  CHECK(root_status_observe(&prior, &page, 130, 62500000, &fresh) ==
        root_status_observe_regressed);
  /* A newer Root generation is a fresh relist, not a regression. */
  page.header.root_generation = 2;
  page.children[2].lifetime_count = 0;
  CHECK(root_status_observe(&prior, &page, 130, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK(fresh.changed);
  CHECK_EQ_U64(fresh.missed_events, 0);
  CHECK(root_status_observe(&prior, nullptr, 0, 0, &fresh) ==
        root_status_observe_regressed);
}

static void test_ring(void) {
  start(production);
  chryso_root_event event = {
      .kind = chryso_root_event_kind_control,
      .child = 0,
      .detail = chryso_root_control_kind_fault_inject,
  };
  memcpy(&prior, &page, sizeof(prior));
  for (size_t i = 0; i < 130; i++) {
    event.ticks = 100 + i;
    CHECK(root_status_publish(&state, &page, 0, nullptr, 0, 100 + i, true,
                              &event, 1));
  }
  CHECK_EQ_U64(page.header.event_head, 131);
  CHECK_EQ_U64(page.header.event_dropped, 3);
  CHECK_EQ_U64(page.events[0].ticks, 227);
  CHECK(page_valid());
  /* This reader saw head 1: events 1 and 2 were overwritten before it looked;
   * Root's count also includes the boot event. */
  root_status_freshness fresh = {};
  CHECK(root_status_observe(&prior, &page, 230, 62500000, &fresh) ==
        root_status_observe_ok);
  CHECK_EQ_U64(fresh.missed_events, 2);
  CHECK_EQ_U64(root_status_missed(5, 130), 0);
  /* Rows are unaffected by the ring wrapping. */
  CHECK(page.children[2].state == chryso_root_child_wire_state_live);
  page.header.event_dropped = 2;
  CHECK(root_status_read(&page, &scratch, &output, sizeof(output), 2) ==
        root_status_read_invalid);
  page.header.event_dropped = 3;
  state.event_head = UINT64_MAX;
  CHECK(
      !root_status_publish(&state, &page, 0, nullptr, 0, 0, false, &event, 1));
  CHECK(state.exhausted);
  CHECK_EQ_U64(page.header.event_head, 131);
}

static void test_saturation(void) {
  start(production);
  root_status_observation *obs = &state.observations[2];
  obs->down_open = true;
  obs->down_since_ticks = 5;
  obs->cumulative_down_ticks = UINT64_MAX - 2;
  root_status_note_restart(&state, 2, 15, true, false);
  CHECK_EQ_U64(obs->cumulative_down_ticks, UINT64_MAX);
  /* A counter that went backwards closes the interval without adding. */
  obs->down_open = true;
  obs->down_since_ticks = 50;
  obs->cumulative_down_ticks = 7;
  root_status_note_restart(&state, 2, 40, true, false);
  CHECK_EQ_U64(obs->cumulative_down_ticks, 7);
  CHECK(!obs->down_open);
}

/* Bit flips and byte bursts over a valid page: every result is bounded and a
 * rejected page never reaches output. */
/* Torn-copy fuzzing of the real writer against the real reader.
 *
 * Three consecutive publications come from root_status_publish. Between the
 * odd and the even sequence the writer's payload stores are plain stores the
 * compiler may reorder, so the model is adversarial: at each instant any
 * prefix of a random permutation of the bytes that publication changes is
 * visible, at byte granularity. The reader starts at one instant, copies its
 * page in three address-ordered chunks at later instants, and rereads the
 * sequence later still. Whatever the interleaving, it must return exactly a
 * published page or leave output untouched. */
enum { fuzz_publications = 3 };

static chryso_root_status_page published[fuzz_publications + 1];
static size_t changed[fuzz_publications][sizeof(chryso_root_status_page)];
static size_t changed_count[fuzz_publications];
static chryso_root_status_page instant;

static uint64_t fuzz_next(uint64_t *seed) {
  *seed ^= *seed << 13;
  *seed ^= *seed >> 7;
  *seed ^= *seed << 17;
  return *seed;
}

static size_t fuzz_below(uint64_t *seed, size_t bound) {
  return (size_t)(fuzz_next(seed) % bound);
}

/* Instants run P0, then for each publication i: odd with 0..n_i of its
 * changed bytes visible, then P(i+1). */
static size_t fuzz_instants(void) {
  size_t total = 1;
  for (size_t i = 0; i < fuzz_publications; i++) {
    total += changed_count[i] + 2;
  }
  return total;
}

static void fuzz_materialize(size_t t, chryso_root_status_page *out) {
  for (size_t i = 0; i < fuzz_publications; i++) {
    if (t == 0) {
      memcpy(out, &published[i], sizeof(*out));
      return;
    }
    t--;
    if (t <= changed_count[i]) {
      memcpy(out, &published[i], sizeof(*out));
      atomic_store(&out->header.seq, atomic_load(&published[i].header.seq) + 1);
      const uint8_t *next = (const uint8_t *)&published[i + 1];
      for (size_t k = 0; k < t; k++) {
        ((uint8_t *)out)[changed[i][k]] = next[changed[i][k]];
      }
      return;
    }
    t -= changed_count[i] + 1;
  }
  memcpy(out, &published[fuzz_publications], sizeof(*out));
}

static void fuzz_build_timeline(void) {
  start(production);
  memcpy(&published[0], &page, sizeof(page));
  root_child_record record = {};
  root_status_note_fault(&state, 2, 6, 2, 0x10, 0x20, 110, true);
  root_record_charge(&record);
  root_status_note_restart(&state, 2, 114, true, false);
  const chryso_root_event pair[2] = {
      fault_event(2, chryso_fault_mr0_valid | chryso_fault_mr1_valid, 110),
      {.ticks = 114,
       .kind = chryso_root_event_kind_restart,
       .child = 2,
       .a = 1},
  };
  CHECK(root_status_publish(&state, &page, 2, &record, 8, 114, true, pair, 2));
  memcpy(&published[1], &page, sizeof(page));
  const chryso_root_event control = {.ticks = 120,
                                     .kind = chryso_root_event_kind_control,
                                     .child = 3,
                                     .detail =
                                         chryso_root_control_kind_fault_inject};
  CHECK(root_status_publish(&state, &page, 3, &(root_child_record){}, 8, 120,
                            true, &control, 1));
  memcpy(&published[2], &page, sizeof(page));
  root_status_note_fault(&state, 5, 6, 1, 0x30, 0, 130, true);
  root_record_charge(&record);
  root_status_note_restart(&state, 5, 131, true, true);
  const chryso_root_event beam[2] = {
      fault_event(5, chryso_fault_mr0_valid, 130),
      {.ticks = 131,
       .kind = chryso_root_event_kind_restart,
       .child = 5,
       .a = 1},
  };
  CHECK(root_status_publish(&state, &page, 5, &record, 64, 131, true, beam, 2));
  memcpy(&published[3], &page, sizeof(page));

  constexpr size_t seq_begin = offsetof(chryso_root_status_page, header.seq);
  for (size_t i = 0; i < fuzz_publications; i++) {
    CHECK(chryso_check_root_status_page((const uint8_t *)&published[i + 1],
                                        sizeof(page)) == chryso_abi_reject_ok);
    const uint8_t *was = (const uint8_t *)&published[i];
    const uint8_t *now = (const uint8_t *)&published[i + 1];
    changed_count[i] = 0;
    for (size_t offset = 0; offset < sizeof(page); offset++) {
      if ((offset < seq_begin || offset >= seq_begin + 8) &&
          was[offset] != now[offset]) {
        changed[i][changed_count[i]++] = offset;
      }
    }
    CHECK(changed_count[i] != 0);
  }
}

static bool fuzz_is_published(const chryso_root_status_page *copy) {
  for (size_t i = 0; i <= fuzz_publications; i++) {
    if (memcmp(copy, &published[i], sizeof(*copy)) == 0) {
      return true;
    }
  }
  return false;
}

static void sort4(size_t v[4]) {
  for (size_t i = 1; i < 4; i++) {
    for (size_t j = i; j > 0 && v[j - 1] > v[j]; j--) {
      const size_t swap = v[j];
      v[j] = v[j - 1];
      v[j - 1] = swap;
    }
  }
}

static void test_torn_copies(void) {
  fuzz_build_timeline();
  const size_t instants = fuzz_instants();
  uint64_t seed = UINT64_C(0xbb67ae8584caa73b);
  size_t accepted = 0;
  size_t torn = 0;
  size_t unpublished = 0;
  for (size_t iteration = 0; iteration < 4096; iteration++) {
    /* A fresh store order for every publication. */
    for (size_t i = 0; i < fuzz_publications; i++) {
      for (size_t k = changed_count[i]; k > 1; k--) {
        const size_t j = fuzz_below(&seed, k);
        const size_t swap = changed[i][k - 1];
        changed[i][k - 1] = changed[i][j];
        changed[i][j] = swap;
      }
    }
    /* Begin, two chunk boundaries and the final reread, in time order. Bias
     * a quarter of the runs onto one publication window. */
    size_t when[4];
    const size_t window = fuzz_below(&seed, 4) == 0 ? 3 : instants;
    const size_t first = fuzz_below(&seed, instants);
    for (size_t i = 0; i < 4; i++) {
      when[i] = first + fuzz_below(&seed, window);
      when[i] = when[i] < instants ? when[i] : instants - 1;
    }
    sort4(when);
    size_t split[2] = {fuzz_below(&seed, sizeof(page) + 1),
                       fuzz_below(&seed, sizeof(page) + 1)};
    if (split[0] > split[1]) {
      const size_t swap = split[0];
      split[0] = split[1];
      split[1] = swap;
    }

    fuzz_materialize(when[0], &instant);
    uint64_t seq = 0;
    if (!root_status_copy_begin(&instant, &scratch, &seq)) {
      unpublished++;
      continue;
    }
    fuzz_materialize(when[1], &instant);
    memcpy((uint8_t *)&scratch + split[0], (const uint8_t *)&instant + split[0],
           split[1] - split[0]);
    fuzz_materialize(when[2], &instant);
    memcpy((uint8_t *)&scratch + split[1], (const uint8_t *)&instant + split[1],
           sizeof(page) - split[1]);
    fuzz_materialize(when[3], &instant);
    memset(&output, 0x7c, sizeof(output));
    const root_status_read_result result =
        root_status_copy_finish(&instant, &scratch, &output, seq);
    if (result == root_status_read_ok) {
      accepted++;
      CHECK(fuzz_is_published(&output));
    } else {
      torn += result == root_status_read_torn;
      CHECK(result == root_status_read_torn ||
            result == root_status_read_invalid);
      CHECK_EQ_U64(((const uint8_t *)&output)[0], 0x7c);
      CHECK_EQ_U64(((const uint8_t *)&output)[sizeof(output) - 1], 0x7c);
    }
  }
  /* Every path was reached, so the asserts above were not vacuous. */
  CHECK(accepted > 0);
  CHECK(torn > 0);
  CHECK(unpublished > 0);
}

static void test_arbitrary_pages(void) {
  start(production);
  uint64_t seed = UINT64_C(0x6a09e667f3bcc909);
  for (size_t i = 0; i < 2048; i++) {
    memcpy(&mutated, &page, sizeof(mutated));
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    const size_t offset = (size_t)(seed % sizeof(mutated));
    const size_t burst = i % 2 == 0 ? 1 : 1 + (size_t)((seed >> 32) % 16);
    for (size_t j = 0; j < burst && offset + j < sizeof(mutated); j++) {
      ((uint8_t *)&mutated)[offset + j] ^= (uint8_t)(seed >> (8 * (j % 8)));
    }
    memset(&output, 0x7c, sizeof(output));
    const root_status_read_result result =
        root_status_read(&mutated, &scratch, &output, sizeof(output), 3);
    CHECK(result == root_status_read_ok || result == root_status_read_invalid ||
          result == root_status_read_torn);
    if (result == root_status_read_ok) {
      CHECK(memcmp(&output, &mutated, sizeof(output)) == 0);
    } else {
      CHECK_EQ_U64(((uint8_t *)&output)[offset], 0x7c);
    }
  }
}

int main(void) {
  test_fault_and_giveup();
  test_reader_bounds();
  test_writer_between_halves();
  test_repeated_sequence_churn();
  test_semantic_rejections();
  test_exhaustion_and_misuse();
  test_unmapped_and_clock();
  test_observe();
  test_ring();
  test_saturation();
  test_arbitrary_pages();
  test_torn_copies();
  return check_finish("root_control");
}
