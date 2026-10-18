/* Root's command decisions. Malformed requests are rejected before a token is
 * spent, so a flood of garbage costs O(1) each and cannot drain the bucket for
 * well-formed callers; every well-formed request, including HELLO, SYNC and
 * one that is later refused or stale, spends a token, so retry storms are
 * bounded too. The bucket keeps its own clock sample and never touches the
 * clock the restart policy decides with. */
#include "command.h"

static_assert(sizeof(((chryso_ctl_command *)nullptr)->args) ==
              root_command_words * sizeof(uint64_t));
static_assert(sizeof(((chryso_ctl_reply *)nullptr)->values) ==
              root_command_words * sizeof(uint64_t));

typedef enum : uint8_t {
  /* Not a declared opcode: bad_arg before a token is spent. */
  class_invalid,
  class_handshake,
  class_barrier,
  class_preconditioned,
  /* Refused until B6 declares pool slots; a placeholder grants nothing. */
  class_slot,
} opcode_class;

typedef struct {
  opcode_class kind;
  /* Words a well-formed request leaves zero, one bit per word. */
  uint8_t unused;
} opcode_row;

static constexpr size_t opcode_count = chryso_pp_opcode_sync + 1;

/* Every declared opcode has a row; unset and gaps stay class_invalid. */
static constexpr opcode_row opcode_rows[opcode_count] = {
    [chryso_pp_opcode_hello] = {class_handshake, 0xe},
    [chryso_pp_opcode_restart_child] = {class_preconditioned, 0},
    [chryso_pp_opcode_start_slot] = {class_slot, 0},
    [chryso_pp_opcode_stop_child] = {class_slot, 0},
    [chryso_pp_opcode_resume_child] = {class_slot, 0},
    [chryso_pp_opcode_reclaim_slot] = {class_slot, 0},
    [chryso_pp_opcode_sync] = {class_barrier, 0xf},
};

static opcode_row row_of(uint64_t label) {
  return label < opcode_count ? opcode_rows[label] : (opcode_row){};
}

void root_command_init(root_command_state *state, uint32_t capacity,
                       uint64_t refill_ticks, uint64_t now, bool now_valid) {
  *state = (root_command_state){
      .refill = root_command_refill_off,
      .capacity = capacity,
      .tokens = capacity,
      .last_ticks = now,
      .fence = root_command_fence_empty,
  };
  if (capacity != 0 && refill_ticks != 0 && now_valid) {
    /* Round up so tokens never arrive faster than the configured rate. */
    state->interval_ticks =
        refill_ticks / capacity + (refill_ticks % capacity != 0);
    state->refill = root_command_refill_running;
  }
}

chryso_ctl_result root_command_frame(root_command_state *state,
                                     unsigned int channel,
                                     unsigned int pp_channel, uint64_t label,
                                     uint64_t count) {
  chryso_ctl_result result = chryso_ctl_result_ok;
  if (channel != pp_channel) {
    result = chryso_ctl_result_refused;
  } else if (count != root_command_words ||
             row_of(label).kind == class_invalid) {
    result = chryso_ctl_result_bad_arg;
  }
  if (result != chryso_ctl_result_ok) {
    state->rejected++;
  }
  return result;
}

/* One clock sample through the refill machine. */
static void refill_step(root_command_state *state, uint64_t now,
                        bool now_valid) {
  switch (state->refill) {
  case root_command_refill_off:
    return;
  case root_command_refill_running:
    if (!now_valid || now < state->last_ticks) {
      state->refill = root_command_refill_off;
      return;
    }
    break;
  }
  const uint64_t due = (now - state->last_ticks) / state->interval_ticks;
  if (due == 0) {
    return;
  }
  const uint64_t room = state->capacity - state->tokens;
  if (due >= room) {
    state->tokens = state->capacity;
    state->last_ticks = now;
    return;
  }
  /* due < room <= UINT32_MAX, and due * interval <= now - last_ticks. */
  state->tokens += (uint32_t)due;
  state->last_ticks += due * state->interval_ticks;
}

/* Freestanding Root has no libc memcpy or memcmp. */
static void copy_words(uint64_t to[static root_command_words],
                       const uint64_t from[static root_command_words]) {
  for (size_t i = 0; i < root_command_words; i++) {
    to[i] = from[i];
  }
}

static bool fence_repeats(const root_command_state *state, uint16_t opcode,
                          const uint64_t args[static root_command_words]) {
  switch (state->fence) {
  case root_command_fence_empty:
    return false;
  case root_command_fence_completed:
    break;
  }
  if (state->memo_opcode != opcode) {
    return false;
  }
  for (size_t i = 0; i < root_command_words; i++) {
    if (state->memo_args[i] != args[i]) {
      return false;
    }
  }
  return true;
}

static void decide_restart(const root_command_state *state,
                           const root_command_view *view,
                           root_command_decision *d) {
  if (fence_repeats(state, d->opcode, d->args)) {
    d->result = chryso_ctl_result_already_done;
    copy_words(d->values, state->memo_values);
    return;
  }
  root_child child = {};
  if (d->args[0] > UINT32_MAX ||
      !root_child_from_raw((unsigned int)d->args[0], view->child_count,
                           view->present_mask, &child)) {
    d->result = chryso_ctl_result_bad_arg;
    return;
  }
  const root_child_record *record = &view->records[child.value];
  if (((view->driver_mask >> child.value) & 1u) == 0 ||
      record->state == root_child_gone || !view->publishing) {
    d->result = chryso_ctl_result_refused;
    return;
  }
  if (d->args[1] != view->root_generation || d->args[2] != view->seq ||
      d->args[3] != record->lifetime_count) {
    d->result = chryso_ctl_result_stale;
    d->values[0] = view->root_generation;
    d->values[1] = view->seq;
    d->values[2] = record->lifetime_count;
    return;
  }
  d->result = chryso_ctl_result_ok;
  d->action = root_command_restart;
  d->child = child;
}

static void tally(root_command_state *state, chryso_ctl_result result) {
  switch (result) {
  case chryso_ctl_result_ok:
  case chryso_ctl_result_already_done:
    state->accepted++;
    return;
  case chryso_ctl_result_rate_limited:
    state->rate_limited++;
    return;
  default:
    state->rejected++;
    return;
  }
}

root_command_decision
root_command_decide(root_command_state *state, const root_command_view *view,
                    uint16_t opcode,
                    const uint64_t args[static root_command_words],
                    uint64_t now, bool now_valid) {
  root_command_decision d = {.opcode = opcode};
  copy_words(d.args, args);
  const opcode_row row = row_of(opcode);

  bool malformed = row.kind == class_invalid;
  for (size_t i = 0; i < root_command_words; i++) {
    malformed |= ((row.unused >> i) & 1u) != 0 && args[i] != 0;
  }
  if (malformed) {
    d.result = chryso_ctl_result_bad_arg;
    tally(state, d.result);
    return d;
  }

  refill_step(state, now, now_valid);
  if (state->tokens == 0) {
    d.result = chryso_ctl_result_rate_limited;
    tally(state, d.result);
    return d;
  }
  state->tokens--;

  switch (row.kind) {
  case class_handshake:
    d.result = args[0] == view->abi_version ? chryso_ctl_result_ok
                                            : chryso_ctl_result_bad_version;
    d.values[0] = view->abi_version;
    d.values[1] = view->root_generation;
    d.values[2] = view->beam_incarnation;
    d.values[3] = view->cntfrq;
    break;
  case class_barrier:
    if (!view->publishing) {
      d.result = chryso_ctl_result_refused;
      break;
    }
    d.result = chryso_ctl_result_ok;
    /* Counted first so the reply includes this SYNC. */
    tally(state, d.result);
    d.values[0] = view->seq;
    d.values[1] = state->accepted;
    d.values[2] = state->rate_limited;
    d.values[3] = state->rejected;
    return d;
  case class_preconditioned:
    decide_restart(state, view, &d);
    break;
  case class_slot:
  case class_invalid:
    d.result = chryso_ctl_result_refused;
    break;
  }
  tally(state, d.result);
  return d;
}

void root_command_complete(root_command_state *state,
                           const root_command_decision *decision) {
  switch (decision->action) {
  case root_command_none:
    return;
  case root_command_restart:
    break;
  }
  state->fence = root_command_fence_completed;
  state->memo_opcode = decision->opcode;
  copy_words(state->memo_args, decision->args);
  copy_words(state->memo_values, decision->values);
}
