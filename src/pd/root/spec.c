/* Root's consumer of the BEAM-owned spec page. Root trusts nothing on the
 * page: it copies it once, validates the copy with the generated checkers,
 * decodes fields from bytes rather than through struct lvalues, and commits a
 * new policy in one assignment. The one-core image serializes Root and the
 * BEAM, so the copy is never concurrent with a write; the marker recheck still
 * rejects a copy a writer could have overlapped. */
#include "spec.h"

#include <stdatomic.h>

static_assert(sizeof(chryso_spec_page) == 4096);
static_assert(chryso_child_count <= 64, "presence is a 64-bit mask");

static constexpr size_t page_size = sizeof(chryso_spec_page);
static constexpr size_t header_size = sizeof(chryso_spec_header);
static constexpr size_t bank_size = sizeof(chryso_spec_bank);
static constexpr size_t active_at =
    offsetof(chryso_spec_page, header.active_bank);
static constexpr size_t generation_at = offsetof(chryso_spec_bank, generation);
static constexpr size_t crc_at = offsetof(chryso_spec_bank, crc32);
static constexpr size_t seq_at = offsetof(chryso_spec_bank, bank_seq);
static constexpr size_t budget_at = offsetof(chryso_spec_bank, budget);
static constexpr size_t desired_at = offsetof(chryso_spec_bank, desired);

typedef enum : uint8_t {
  /* bank_seq 0, or a sealed generation 0: nothing to apply, nothing wrong. */
  candidate_empty,
  candidate_rejected,
  candidate_same,
  candidate_newer,
  /* The other bank is not read once the active bank decides. */
  candidate_skipped,
} candidate_kind;

static constexpr size_t candidate_kinds = 5;

typedef enum : uint8_t {
  choose_none,
  choose_active,
  choose_other,
} choice;

typedef struct {
  root_spec_outcome outcome;
  choice chosen;
} selection;

/* What Root does for each (active bank, other bank) pair. The active bank
 * wins whenever it can be used; the other bank is a fallback, read only when
 * the active one is empty or rejected. Every cell is listed: an unlisted one
 * would silently read as "absent, none". */
static constexpr selection selections[candidate_kinds][candidate_kinds] = {
    [candidate_empty] =
        {
            [candidate_empty] = {root_spec_absent, choose_none},
            /* A first commit in progress: nothing was ever published. */
            [candidate_rejected] = {root_spec_absent, choose_none},
            [candidate_same] = {root_spec_unchanged, choose_other},
            [candidate_newer] = {root_spec_applied, choose_other},
            [candidate_skipped] = {root_spec_absent, choose_none},
        },
    [candidate_rejected] =
        {
            [candidate_empty] = {root_spec_kept, choose_none},
            [candidate_rejected] = {root_spec_kept, choose_none},
            [candidate_same] = {root_spec_unchanged, choose_other},
            [candidate_newer] = {root_spec_applied, choose_other},
            [candidate_skipped] = {root_spec_kept, choose_none},
        },
    [candidate_same] =
        {
            [candidate_empty] = {root_spec_unchanged, choose_active},
            [candidate_rejected] = {root_spec_unchanged, choose_active},
            [candidate_same] = {root_spec_unchanged, choose_active},
            [candidate_newer] = {root_spec_unchanged, choose_active},
            [candidate_skipped] = {root_spec_unchanged, choose_active},
        },
    [candidate_newer] =
        {
            [candidate_empty] = {root_spec_applied, choose_active},
            [candidate_rejected] = {root_spec_applied, choose_active},
            [candidate_same] = {root_spec_applied, choose_active},
            [candidate_newer] = {root_spec_applied, choose_active},
            [candidate_skipped] = {root_spec_applied, choose_active},
        },
    /* The active bank is always read. */
    [candidate_skipped] =
        {
            [candidate_empty] = {root_spec_kept, choose_none},
            [candidate_rejected] = {root_spec_kept, choose_none},
            [candidate_same] = {root_spec_kept, choose_none},
            [candidate_newer] = {root_spec_kept, choose_none},
            [candidate_skipped] = {root_spec_kept, choose_none},
        },
};

typedef struct {
  candidate_kind kind;
  chryso_root_spec_reject reason;
  uint64_t generation;
  root_spec_policy policy;
} candidate;

void root_spec_init(root_spec_state *state) { *state = (root_spec_state){}; }

static const uint8_t *bank_bytes(const chryso_spec_page *page, size_t bank) {
  return (const uint8_t *)page + header_size + bank * bank_size;
}

/* Another PD writes this page, so volatile makes every byte one real load
 * between the marker reads. It orders nothing: the acquire loads and fence in
 * snapshot() do. */
static void copy_page(const chryso_spec_page *shared, chryso_spec_page *out) {
  const volatile uint8_t *from = (const volatile uint8_t *)shared;
  uint8_t *to = (uint8_t *)out;
  for (size_t i = 0; i < page_size; i++) {
    to[i] = from[i];
  }
}

/* True when the markers were stable across the copy and the copy holds them. */
static bool snapshot(const chryso_spec_page *shared, chryso_spec_page *out) {
  const uint32_t active =
      atomic_load_explicit(&shared->header.active_bank, memory_order_acquire);
  const uint32_t seq0 =
      atomic_load_explicit(&shared->banks[0].bank_seq, memory_order_acquire);
  const uint32_t seq1 =
      atomic_load_explicit(&shared->banks[1].bank_seq, memory_order_acquire);
  copy_page(shared, out);
  atomic_thread_fence(memory_order_acquire);
  return atomic_load_explicit(&shared->header.active_bank,
                              memory_order_relaxed) == active &&
         atomic_load_explicit(&shared->banks[0].bank_seq,
                              memory_order_relaxed) == seq0 &&
         atomic_load_explicit(&shared->banks[1].bank_seq,
                              memory_order_relaxed) == seq1 &&
         chryso_abi_load_u32le((const uint8_t *)out + active_at) == active &&
         chryso_abi_load_u32le(bank_bytes(out, 0) + seq_at) == seq0 &&
         chryso_abi_load_u32le(bank_bytes(out, 1) + seq_at) == seq1;
}

static bool same_content(const root_spec_policy *a, const root_spec_policy *b) {
  for (size_t c = 0; c < chryso_child_count; c++) {
    if (a->budget[c] != b->budget[c] || a->desired[c] != b->desired[c]) {
      return false;
    }
  }
  return true;
}

static candidate rejected(chryso_root_spec_reject reason, uint64_t generation) {
  return (candidate){
      .kind = candidate_rejected, .reason = reason, .generation = generation};
}

static chryso_root_spec_reject bank_reason(chryso_abi_reject verdict) {
  switch (verdict) {
  case chryso_abi_reject_torn:
    return chryso_root_spec_reject_bank_torn;
  case chryso_abi_reject_checksum:
    return chryso_root_spec_reject_bank_checksum;
  case chryso_abi_reject_ok:
  case chryso_abi_reject_magic:
  case chryso_abi_reject_version:
  case chryso_abi_reject_size:
  case chryso_abi_reject_reserved:
  case chryso_abi_reject_unknown_kind:
  case chryso_abi_reject_range:
    break;
  }
  return chryso_root_spec_reject_bank_malformed;
}

/* Classifies one bank of the validated copy against the retained policy. */
static candidate evaluate(const chryso_spec_page *page, size_t bank,
                          uint64_t present_mask,
                          const root_spec_policy *retained) {
  const uint8_t *at = bank_bytes(page, bank);
  if (chryso_abi_load_u32le(at + seq_at) == 0) {
    return (candidate){.kind = candidate_empty};
  }
  const chryso_abi_reject verdict =
      chryso_check_spec_bank((const uint8_t *)page, page_size, bank);
  if (verdict != chryso_abi_reject_ok) {
    return rejected(bank_reason(verdict), 0);
  }
  const uint64_t generation = chryso_abi_load_u64le(at + generation_at);
  if (generation == 0) {
    return (candidate){.kind = candidate_empty};
  }
  candidate result = {
      .kind = candidate_newer,
      .generation = generation,
      .policy = {.generation = generation,
                 .bank = (uint32_t)bank,
                 .crc32 = chryso_abi_load_u32le(at + crc_at)},
  };
  for (size_t c = 0; c < chryso_child_count; c++) {
    const uint32_t budget =
        chryso_abi_load_u32le(at + budget_at + c * sizeof(uint32_t));
    const uint8_t raw = chryso_abi_load_u8(at + desired_at + c);
    const bool present = ((present_mask >> c) & 1u) != 0;
    if (!present && (budget != 0 || raw != 0)) {
      return rejected(chryso_root_spec_reject_undeclared_child, generation);
    }
    result.policy.budget[c] = budget;
    result.policy.desired[c] = raw;
  }
  if (generation < retained->generation) {
    return rejected(chryso_root_spec_reject_regressed, generation);
  }
  if (generation == retained->generation) {
    if (!same_content(&result.policy, retained)) {
      return rejected(chryso_root_spec_reject_conflict, generation);
    }
    result.kind = candidate_same;
  }
  return result;
}

/* Reports reason once per (reason, generation, bank); no reason clears it so
 * the same fault seen again later is reported again. */
static root_spec_result finish(root_spec_state *state,
                               root_spec_outcome outcome,
                               chryso_root_spec_reject reason,
                               uint64_t generation, uint32_t bank) {
  root_spec_result result = {.outcome = outcome,
                             .reason = reason,
                             .generation = generation,
                             .bank = bank};
  if (reason == chryso_root_spec_reject_unset) {
    state->last_reason = chryso_root_spec_reject_unset;
    state->last_generation = 0;
    state->last_bank = 0;
    return result;
  }
  result.record = reason != state->last_reason ||
                  generation != state->last_generation ||
                  bank != state->last_bank;
  state->last_reason = reason;
  state->last_generation = generation;
  state->last_bank = bank;
  return result;
}

root_spec_result root_spec_consume(root_spec_state *state,
                                   const chryso_spec_page *shared,
                                   chryso_spec_page *scratch,
                                   uint64_t present_mask) {
  if (state == nullptr || scratch == nullptr) {
    return (root_spec_result){.outcome = root_spec_absent};
  }
  if (shared == nullptr) {
    return finish(state, root_spec_absent, chryso_root_spec_reject_unset, 0, 0);
  }
  if (!snapshot(shared, scratch)) {
    return finish(state, root_spec_kept, chryso_root_spec_reject_unstable_copy,
                  0, 0);
  }
  const uint8_t *bytes = (const uint8_t *)scratch;
  if (chryso_abi_all_zero(bytes, header_size)) {
    return finish(state, root_spec_absent, chryso_root_spec_reject_unset, 0, 0);
  }
  if (chryso_check_spec_header(bytes, page_size) != chryso_abi_reject_ok) {
    return finish(state, root_spec_kept, chryso_root_spec_reject_header, 0, 0);
  }
  /* The header check bounds active_bank to 0 or 1. */
  const size_t active = chryso_abi_load_u32le(bytes + active_at);
  const candidate first =
      evaluate(scratch, active, present_mask, &state->retained);
  const bool fallback =
      first.kind == candidate_empty || first.kind == candidate_rejected;
  const candidate second =
      fallback ? evaluate(scratch, 1 - active, present_mask, &state->retained)
               : (candidate){.kind = candidate_skipped};
  const selection pick = selections[first.kind][second.kind];
  /* Only the active bank's rejection is reported: a fallback bank mid-write
   * is the normal state of a commit in progress. */
  const bool rejected_active = first.kind == candidate_rejected;
  const chryso_root_spec_reject reason =
      rejected_active ? first.reason : chryso_root_spec_reject_unset;
  const uint64_t generation = rejected_active ? first.generation : 0;
  if (pick.outcome == root_spec_applied) {
    state->retained =
        pick.chosen == choose_active ? first.policy : second.policy;
  }
  return finish(state, pick.outcome, reason, generation, (uint32_t)active);
}

uint32_t root_spec_budget(const root_spec_state *state, unsigned int child,
                          uint32_t compiled, uint32_t hard_max) {
  if (child >= chryso_child_count || state->retained.generation == 0) {
    return compiled;
  }
  const uint32_t budget = state->retained.budget[child];
  if (budget == 0) {
    return compiled;
  }
  return budget < hard_max ? budget : hard_max;
}

chryso_root_desired root_spec_desired(const root_spec_state *state,
                                      unsigned int child, uint64_t slot_mask) {
  if (child >= chryso_child_count || ((slot_mask >> child) & 1u) == 0 ||
      state->retained.generation == 0) {
    return chryso_root_desired_running;
  }
  /* Compared as a byte so an undeclared value never becomes an enum. */
  return state->retained.desired[child] == chryso_root_desired_stopped
             ? chryso_root_desired_stopped
             : chryso_root_desired_running;
}

const char *root_spec_reject_name(chryso_root_spec_reject reason) {
  switch (reason) {
  case chryso_root_spec_reject_unset:
    return "unset";
  case chryso_root_spec_reject_header:
    return "header";
  case chryso_root_spec_reject_bank_torn:
    return "bank-torn";
  case chryso_root_spec_reject_bank_checksum:
    return "bank-checksum";
  case chryso_root_spec_reject_bank_malformed:
    return "bank-malformed";
  case chryso_root_spec_reject_undeclared_child:
    return "undeclared-child";
  case chryso_root_spec_reject_regressed:
    return "regressed";
  case chryso_root_spec_reject_conflict:
    return "conflict";
  case chryso_root_spec_reject_unstable_copy:
    return "unstable-copy";
  }
  return "invalid";
}
