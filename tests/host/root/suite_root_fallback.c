/*
 * Root's fallback laws as properties (B3.6). The pure units are composed the
 * way main.c composes them: refresh the spec, plan, commit, publish. That
 * composition is restated here, so these properties cover the units and
 * their contract; main.c's own wiring is covered by orch-fallback-smoke.
 *
 *   spec model   after every refresh, the retained policy equals an
 *                independent bank model (empty, invalid or sealed; the active
 *                bank if newer or the same, otherwise the other if newer),
 *                over commits stopped after any step and corrupted banks
 *   P1 identity  with no spec ever applied, decisions equal a plain counter
 *                against the compiled budget
 *   P2 cap       the effective budget is min(spec ?: compiled, hard_max)
 *   P3, P6       a gone child ignores everything and keeps its record;
 *                lifetime counts never decrease (checked every step)
 *   P4           substitution: replacing each accepted RESTART_CHILD of d by
 *                a direct restart of d and deleting every other command
 *                leaves d's outcomes and record unchanged, so commands reach a
 *                driver only through the shared budget path
 *   P5           a refresh that applies nothing keeps the policy
 *   P7           only an accepted RESTART_CHILD acts; tokens stay within
 *                capacity; every command call is tallied once
 */
#include "check.h"
#include "command.h"
#include "prop.h"
#include "restart_plan.h"
#include "root_policy.h"
#include "spec.h"
#include "spec_writer.h"
#include "status.h"

#include <stdatomic.h>
#include <string.h>

/* Serial, timer, blk, eth and beam; the crasher (4) is absent. */
static constexpr uint64_t present = UINT64_C(0x2f);
static constexpr uint64_t drivers = UINT64_C(0x0f);
static constexpr unsigned int beam = 5;
static constexpr unsigned int driver_budget = 3;
static constexpr unsigned int beam_budget = 2;
/* runtime_abi.h's chryso_control_hard_budget_max, which tests/host lacks. */
static constexpr unsigned int hard_max = 64;
static constexpr uint64_t entry = 0x200000;
static constexpr unsigned int pp = 11;
static constexpr uint32_t tokens = 4;
static constexpr uint64_t refill_ticks = 400;
static constexpr size_t max_events = 192;
static constexpr unsigned int cases = 400;

static_assert(chryso_child_count > beam);

static unsigned int compiled_of(unsigned int child) {
  return child == beam ? beam_budget : driver_budget;
}

static bool is_present(unsigned int child) {
  return child < chryso_child_count && ((present >> child) & 1u) != 0;
}

/* --- events ---------------------------------------------------------- */

typedef enum : uint8_t {
  ev_fault,
  /* variant: 0 fresh preconditions, 1 stale seq, 2 verbatim repeat. */
  ev_restart_cmd,
  ev_hello,
  ev_sync,
  /* variant: 0 short frame, 1 nonzero unused word, 2 unknown opcode. */
  ev_malformed,
  ev_slot_cmd,
  /* variant: steps run, 0..5; 5 is a whole commit. value picks budgets. */
  ev_spec_commit,
  ev_spec_corrupt,
  /* value: ticks to advance. */
  ev_tick,
  /* A restart decision with no command around it, as the debug-restart
   * notification makes. Only the P4 substitution produces it. */
  ev_direct,
  ev_kind_count,
} ev_kind;

typedef struct {
  ev_kind kind;
  uint8_t child;
  uint8_t variant;
  uint32_t value;
} event;

static const char *const ev_names[ev_kind_count] = {
    [ev_fault] = "fault",         [ev_restart_cmd] = "restart_cmd",
    [ev_hello] = "hello",         [ev_sync] = "sync",
    [ev_malformed] = "malformed", [ev_slot_cmd] = "slot_cmd",
    [ev_spec_commit] = "spec",    [ev_spec_corrupt] = "corrupt",
    [ev_tick] = "tick",           [ev_direct] = "direct",
};

/* Budget values a commit may name: 0 means "use the compiled budget", 70 is
 * above the hard maximum. */
static constexpr uint32_t budget_choices[] = {0, 1, 2, 3, 5, 70};
static constexpr size_t budget_choice_count =
    sizeof(budget_choices) / sizeof(budget_choices[0]);

static uint32_t budget_for(uint32_t value, unsigned int child) {
  return budget_choices[(value >> (3 * child)) % budget_choice_count];
}

/* --- the composition under test --------------------------------------- */

static chryso_spec_page spec_page;
static chryso_spec_page spec_scratch;
static chryso_root_status_page status_page;

typedef struct {
  bool mapped;
  root_child_record records[chryso_child_count];
  root_spec_state spec;
  root_command_state cmd;
  root_status_state status;
  uint64_t generation;
  uint64_t now;
  uint64_t calls;
  bool have_last;
  uint16_t last_opcode;
  uint64_t last_args[root_command_words];
} sut;

/* What one event did, as the reference needs it. */
typedef struct {
  bool refreshed;
  bool decided;
  unsigned int child;
  root_restart_action action;
  /* A command that reached the restart path for child. */
  bool accepted_restart;
  chryso_ctl_result result;
  /* The retained policy after the step. */
  uint64_t retained_generation;
  uint32_t retained_budget[chryso_child_count];
} step_out;

typedef struct {
  unsigned int giveups;
  unsigned int applied;
  unsigned int rate_limited;
  unsigned int stale;
  unsigned int already_done;
  unsigned int accepted_restarts;
  unsigned int ignored;
} coverage;

static coverage cover;

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

static void sut_init(sut *s, bool mapped) {
  *s = (sut){.mapped = mapped, .now = 1000};
  memset(&spec_page, 0, sizeof(spec_page));
  memset(&status_page, 0, sizeof(status_page));
  root_spec_init(&s->spec);
  root_command_init(&s->cmd, tokens, refill_ticks, s->now, true);
  uint32_t budget[chryso_child_count] = {};
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    budget[c] = compiled_of(c);
  }
  root_status_init(&s->status, mapped ? &status_page : nullptr, present, s->now,
                   62500000, budget);
}

/* The record checks that hold after every decision: P3 and P6. */
static bool record_laws(const root_child_record *before,
                        const root_child_record *after,
                        root_restart_action action) {
  bool ok = after->lifetime_count >= before->lifetime_count;
  ok &= after->lifetime_count <= hard_max;
  if (before->state == root_child_gone) {
    ok &= action == root_restart_ignore_gone;
    ok &= after->state == root_child_gone &&
          after->lifetime_count == before->lifetime_count &&
          after->window_count == before->window_count;
  }
  return ok;
}

static bool law_failed;

/* main.c's root_restart_child plus the publish at the end of its callback. */
static root_restart_action sut_decide(sut *s, unsigned int child) {
  const root_spec_policy kept = s->spec.retained;
  const root_spec_result refreshed = root_spec_consume(
      &s->spec, s->mapped ? &spec_page : nullptr, &spec_scratch, present);
  if (refreshed.outcome == root_spec_applied) {
    cover.applied++;
  } else if (!same_policy(&kept, &s->spec.retained)) {
    law_failed = true; /* P5 */
  }
  root_child_record *record = &s->records[child];
  const root_child_record before = *record;
  const root_restart_inputs in = {
      .child = child,
      .compiled = compiled_of(child),
      .hard_max = hard_max,
      .entry = entry,
      .clock_available = true,
      .now_ticks = s->now,
  };
  const root_restart_plan_result plan =
      root_restart_plan(&s->spec, record, &in);
  switch (plan.action) {
  case root_restart_ignore_gone:
    cover.ignored++;
    break;
  case root_restart_resume:
    *record = plan.next;
    root_status_note_restart(&s->status, child, s->now, true, child == beam);
    break;
  case root_restart_giveup_budget_exhausted:
  case root_restart_giveup_window_exhausted:
  case root_restart_giveup_lifetime_exhausted:
  case root_restart_giveup_no_entry:
    cover.giveups++;
    root_record_give_up(record);
    root_status_note_gone(&s->status, child, s->now, true);
    break;
  }
  if (!record_laws(&before, record, plan.action)) {
    law_failed = true;
  }
  if (s->mapped && s->status.enabled) {
    const chryso_root_event outcome = {
        .ticks = s->now,
        .kind = chryso_root_event_kind_restart,
        .child = (uint8_t)child,
        .a = record->lifetime_count,
    };
    (void)root_status_publish(&s->status, &status_page, child, record, s->now,
                              true, &outcome, 1);
  }
  return plan.action;
}

/* main.c's protected() and root_command_restart_child. */
static void sut_command(sut *s, uint16_t opcode, uint64_t count,
                        const uint64_t args[static root_command_words],
                        step_out *out) {
  s->calls++;
  const chryso_ctl_result framed =
      root_command_frame(&s->cmd, pp, pp, opcode, count);
  if (framed != chryso_ctl_result_ok) {
    out->result = framed;
    return;
  }
  const root_command_view view = {
      .abi_version = chryso_abi_version,
      .root_generation = s->status.root_generation,
      .beam_incarnation = s->status.beam_incarnation,
      .cntfrq = s->status.frequency,
      .seq = s->status.seq,
      .publishing = s->mapped && s->status.enabled,
      .present_mask = present,
      .driver_mask = drivers,
      .child_count = chryso_child_count,
      .records = s->records,
  };
  root_command_decision d =
      root_command_decide(&s->cmd, &view, opcode, args, s->now, true);
  out->result = d.result;
  switch (d.result) {
  case chryso_ctl_result_rate_limited:
    cover.rate_limited++;
    break;
  case chryso_ctl_result_stale:
    cover.stale++;
    break;
  case chryso_ctl_result_already_done:
    cover.already_done++;
    break;
  default:
    break;
  }
  /* P7: only an accepted RESTART_CHILD carries an action. */
  if (d.action != root_command_none &&
      (opcode != chryso_pp_opcode_restart_child ||
       d.result != chryso_ctl_result_ok)) {
    law_failed = true;
  }
  if (d.action == root_command_restart) {
    const unsigned int child = d.child.value;
    const root_restart_action action = sut_decide(s, child);
    out->refreshed = true;
    out->decided = true;
    out->accepted_restart = true;
    out->child = child;
    out->action = action;
    if (action == root_restart_ignore_gone) {
      d.result = chryso_ctl_result_refused;
    } else {
      cover.accepted_restarts++;
      d.values[0] = action == root_restart_resume
                        ? chryso_root_event_kind_restart
                        : chryso_root_event_kind_giveup;
      d.values[2] = s->records[child].lifetime_count;
      d.values[3] = s->status.seq;
      root_command_complete(&s->cmd, &d);
    }
  }
  if (s->cmd.tokens > tokens) {
    law_failed = true; /* P7 */
  }
  s->have_last = true;
  s->last_opcode = opcode;
  memcpy(s->last_args, args, sizeof(s->last_args));
}

static void sut_spec_commit(sut *s, unsigned int steps, uint32_t value) {
  uint32_t budget[chryso_child_count] = {};
  uint8_t desired[chryso_child_count] = {};
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    if (is_present(c)) {
      budget[c] = budget_for(value, c);
      desired[c] = chryso_root_desired_running;
    }
  }
  spec_writer writer = {};
  spec_writer_init(&writer);
  if (spec_writer_begin(&writer, &spec_page, ++s->generation, budget,
                        desired) != spec_write_ok) {
    return;
  }
  for (unsigned int i = 0; i < steps && i < spec_writer_step_count; i++) {
    if (spec_writer_step(&writer, (spec_writer_step_kind)i) != spec_write_ok) {
      law_failed = true;
      return;
    }
  }
}

static uint32_t active_bank(void) {
  return atomic_load(&spec_page.header.active_bank);
}

static bool spec_header_written(void) {
  const uint8_t *bytes = (const uint8_t *)&spec_page;
  for (size_t i = 0; i < sizeof(chryso_spec_header); i++) {
    if (bytes[i] != 0) {
      return true;
    }
  }
  return false;
}

/* Flips one budget byte of the active bank without resealing it. */
static void sut_spec_corrupt(void) {
  if (!spec_header_written()) {
    return;
  }
  uint8_t *bank = (uint8_t *)&spec_page.banks[active_bank()];
  bank[offsetof(chryso_spec_bank, budget)] ^= 0x40;
}

static step_out sut_step(sut *s, const event *e) {
  step_out out = {};
  switch (e->kind) {
  case ev_fault:
    out.child = e->child;
    out.action = sut_decide(s, e->child);
    out.refreshed = true;
    out.decided = true;
    break;
  case ev_restart_cmd: {
    uint64_t args[root_command_words] = {e->child, s->status.root_generation,
                                         s->status.seq,
                                         s->records[e->child].lifetime_count};
    uint16_t opcode = chryso_pp_opcode_restart_child;
    if (e->variant == 1) {
      args[2] = s->status.seq + 2;
    } else if (e->variant == 2 && s->have_last) {
      opcode = s->last_opcode;
      memcpy(args, s->last_args, sizeof(args));
    }
    sut_command(s, opcode, root_command_words, args, &out);
    break;
  }
  case ev_hello: {
    const uint64_t args[root_command_words] = {chryso_abi_version};
    sut_command(s, chryso_pp_opcode_hello, root_command_words, args, &out);
    break;
  }
  case ev_sync: {
    const uint64_t args[root_command_words] = {};
    sut_command(s, chryso_pp_opcode_sync, root_command_words, args, &out);
    break;
  }
  case ev_malformed: {
    uint64_t args[root_command_words] = {};
    switch (e->variant) {
    case 0:
      sut_command(s, chryso_pp_opcode_sync, root_command_words - 1, args, &out);
      break;
    case 1:
      args[3] = 1;
      sut_command(s, chryso_pp_opcode_sync, root_command_words, args, &out);
      break;
    default:
      sut_command(s, 0x7777, root_command_words, args, &out);
      break;
    }
    break;
  }
  case ev_slot_cmd: {
    const uint64_t args[root_command_words] = {e->child};
    sut_command(s, chryso_pp_opcode_sync + 1, root_command_words, args, &out);
    break;
  }
  case ev_spec_commit:
    sut_spec_commit(s, e->variant, e->value);
    break;
  case ev_spec_corrupt:
    sut_spec_corrupt();
    break;
  case ev_tick:
    s->now += e->value;
    break;
  case ev_direct:
    out.child = e->child;
    out.action = sut_decide(s, e->child);
    out.refreshed = true;
    out.decided = true;
    break;
  case ev_kind_count:
    break;
  }
  out.retained_generation = s->spec.retained.generation;
  memcpy(out.retained_budget, s->spec.retained.budget,
         sizeof(out.retained_budget));
  return out;
}

/* --- the reference model ---------------------------------------------- */

typedef enum : uint8_t {
  bank_empty,
  bank_invalid,
  bank_sealed,
} bank_state;

typedef struct {
  bank_state state;
  /* Each corruption flips the same bit, so an even count restores it. */
  bool flipped;
  uint64_t generation;
  uint32_t budget[chryso_child_count];
} model_bank;

typedef struct {
  bool mapped;
  bool header;
  unsigned int active;
  model_bank banks[2];
  uint64_t retained_generation;
  uint32_t retained_budget[chryso_child_count];
  uint64_t generation;
  unsigned int count[chryso_child_count];
  bool gone[chryso_child_count];
} model;

typedef enum : uint8_t {
  pick_none,
  pick_same,
  pick_newer,
} pick;

static pick model_classify(const model *m, const model_bank *b) {
  if (b->state != bank_sealed || b->flipped ||
      b->generation < m->retained_generation) {
    return pick_none;
  }
  if (b->generation > m->retained_generation) {
    return pick_newer;
  }
  return memcmp(b->budget, m->retained_budget, sizeof(b->budget)) == 0
             ? pick_same
             : pick_none;
}

static void model_adopt(model *m, const model_bank *b) {
  m->retained_generation = b->generation;
  memcpy(m->retained_budget, b->budget, sizeof(m->retained_budget));
}

static void model_refresh(model *m) {
  if (!m->mapped || !m->header) {
    return;
  }
  const model_bank *first = &m->banks[m->active];
  const model_bank *second = &m->banks[1 - m->active];
  switch (model_classify(m, first)) {
  case pick_newer:
    model_adopt(m, first);
    return;
  case pick_same:
    return;
  case pick_none:
    break;
  }
  if (model_classify(m, second) == pick_newer) {
    model_adopt(m, second);
  }
}

static unsigned int model_budget(const model *m, unsigned int child) {
  const uint32_t named =
      m->retained_generation != 0 ? m->retained_budget[child] : 0;
  if (named == 0) {
    return compiled_of(child);
  }
  return named < hard_max ? named : hard_max;
}

static root_restart_action model_decide(model *m, unsigned int child) {
  if (m->gone[child]) {
    return root_restart_ignore_gone;
  }
  if (m->count[child] >= model_budget(m, child)) {
    m->gone[child] = true;
    return root_restart_giveup_budget_exhausted;
  }
  m->count[child]++;
  return root_restart_resume;
}

static void model_spec_commit(model *m, unsigned int steps, uint32_t value) {
  m->generation++;
  if (!m->header) {
    m->header = true;
    m->active = 0;
  }
  if (steps == 0) {
    return;
  }
  model_bank *target = &m->banks[1 - m->active];
  target->flipped = false;
  if (steps < 4) {
    target->state = bank_invalid;
    return;
  }
  target->state = bank_sealed;
  target->generation = m->generation;
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    target->budget[c] = is_present(c) ? budget_for(value, c) : 0;
  }
  if (steps >= 5) {
    m->active = 1 - m->active;
  }
}

static void model_spec_corrupt(model *m) {
  if (m->header) {
    m->banks[m->active].flipped = !m->banks[m->active].flipped;
  }
}

/* --- properties ------------------------------------------------------- */

typedef struct {
  bool mapped;
  /* Driver whose outcomes P4 compares. */
  unsigned int driver;
} prop_ctx;

/* What a run leaves behind: every step's output and the final records. */
typedef struct {
  step_out outs[max_events];
  root_child_record records[chryso_child_count];
  uint64_t retained_generation;
} run_result;

static void run_sut(const event *events, size_t n, bool mapped, run_result *r) {
  static sut s;
  sut_init(&s, mapped);
  for (size_t i = 0; i < n; i++) {
    r->outs[i] = sut_step(&s, &events[i]);
    /* P7: every call is tallied exactly once. */
    if (s.cmd.accepted + s.cmd.rate_limited + s.cmd.rejected != s.calls) {
      law_failed = true;
    }
  }
  memcpy(r->records, s.records, sizeof(r->records));
  r->retained_generation = s.spec.retained.generation;
}

/* The spec model and P1/P2: after every refresh the retained policy equals
 * the bank model's, and every decision equals the reference counter against
 * the model's budget. With no commit ever closed (P1), the model never
 * adopts anything, so that budget is the compiled one. */
static bool model_holds(const void *items, size_t n, void *raw) {
  const event *events = items;
  const prop_ctx *ctx = raw;
  static run_result r;
  law_failed = false;
  run_sut(events, n, ctx->mapped, &r);
  model m = {.mapped = ctx->mapped};
  bool ok = !law_failed;
  for (size_t i = 0; i < n && ok; i++) {
    switch (events[i].kind) {
    case ev_spec_commit:
      model_spec_commit(&m, events[i].variant, events[i].value);
      break;
    case ev_spec_corrupt:
      model_spec_corrupt(&m);
      break;
    default:
      break;
    }
    const step_out *out = &r.outs[i];
    if (!out->refreshed) {
      continue;
    }
    model_refresh(&m);
    ok &= out->retained_generation == m.retained_generation;
    if (m.retained_generation != 0) {
      ok &= memcmp(out->retained_budget, m.retained_budget,
                   sizeof(m.retained_budget)) == 0;
    }
    ok &= out->action == model_decide(&m, out->child);
  }
  return ok;
}

/* P4: accepted restarts become direct restarts, every other command goes,
 * and nothing a driver can observe changes. */
static bool substitution_holds(const void *items, size_t n, void *raw) {
  const event *events = items;
  const prop_ctx *ctx = raw;
  static run_result full;
  static run_result plain;
  static event kept[max_events];
  law_failed = false;
  run_sut(events, n, ctx->mapped, &full);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    switch (events[i].kind) {
    case ev_restart_cmd:
    case ev_hello:
    case ev_sync:
    case ev_malformed:
    case ev_slot_cmd:
      if (full.outs[i].accepted_restart) {
        kept[k++] =
            (event){.kind = ev_direct, .child = (uint8_t)full.outs[i].child};
      }
      break;
    default:
      kept[k++] = events[i];
      break;
    }
  }
  run_sut(kept, k, ctx->mapped, &plain);
  if (law_failed || full.retained_generation != plain.retained_generation) {
    return false;
  }
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    const root_child_record *a = &full.records[c];
    const root_child_record *b = &plain.records[c];
    if (a->lifetime_count != b->lifetime_count || a->state != b->state) {
      return false;
    }
  }
  /* The driver's outcomes, in order, are the same in both runs. */
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (!full.outs[i].decided || full.outs[i].child != ctx->driver) {
      continue;
    }
    while (j < k &&
           !(plain.outs[j].decided && plain.outs[j].child == ctx->driver)) {
      j++;
    }
    if (j == k || plain.outs[j].action != full.outs[i].action) {
      return false;
    }
    j++;
  }
  return true;
}

/* --- generation ------------------------------------------------------- */

static unsigned int pick_child(prop_rng *rng, unsigned int driver) {
  static constexpr unsigned int children[] = {0, 1, 2, 3, beam};
  if (prop_chance(rng, 50)) {
    return driver;
  }
  return children[prop_below(rng, sizeof(children) / sizeof(children[0]))];
}

/* legacy: no commit ever reaches close, so no spec is ever applied. */
static size_t generate(prop_rng *rng, event *out, unsigned int driver,
                       bool legacy) {
  const size_t n = 8 + prop_below(rng, max_events - 8);
  for (size_t i = 0; i < n; i++) {
    event e = {.child = (uint8_t)pick_child(rng, driver)};
    const size_t roll = prop_below(rng, 100);
    if (roll < 25) {
      e.kind = ev_fault;
    } else if (roll < 45) {
      e.kind = ev_restart_cmd;
      e.variant = (uint8_t)prop_below(rng, 3);
    } else if (roll < 52) {
      e.kind = ev_hello;
    } else if (roll < 60) {
      e.kind = ev_sync;
    } else if (roll < 66) {
      e.kind = ev_malformed;
      e.variant = (uint8_t)prop_below(rng, 3);
    } else if (roll < 69) {
      e.kind = ev_slot_cmd;
    } else if (roll < 79) {
      e.kind = ev_spec_commit;
      e.variant =
          (uint8_t)(legacy ? prop_below(rng, 4)
                           : (prop_chance(rng, 70) ? spec_writer_step_count
                                                   : prop_below(rng, 5)));
      e.value = (uint32_t)prop_next(rng);
    } else if (roll < 84) {
      e.kind = ev_spec_corrupt;
    } else {
      e.kind = ev_tick;
      e.value = (uint32_t)prop_below(rng, 3 * refill_ticks);
    }
    out[i] = e;
  }
  return n;
}

static void print_events(const event *events, size_t n) {
  for (size_t i = 0; i < n; i++) {
    fprintf(stderr, "  %zu: %s child=%u variant=%u value=%u\n", i,
            ev_names[events[i].kind], events[i].child, events[i].variant,
            events[i].value);
  }
}

static void run_property(const char *name, prop_holds_fn *holds, uint64_t seed,
                         bool legacy, bool mapped) {
  static event events[max_events];
  static event scratch[max_events];
  prop_rng rng = prop_seed(seed);
  for (unsigned int c = 0; c < cases; c++) {
    const unsigned int driver = (unsigned int)prop_below(&rng, 4);
    prop_ctx ctx = {.mapped = mapped, .driver = driver};
    size_t n = generate(&rng, events, driver, legacy);
    if (holds(events, n, &ctx)) {
      continue;
    }
    n = prop_shrink(events, n, sizeof(events[0]), scratch, holds, &ctx);
    fprintf(stderr,
            "%s failed: seed=0x%016" PRIx64 " case=%u driver=%u mapped=%d\n",
            name, seed, c, driver, mapped);
    print_events(events, n);
    CHECK(false);
    return;
  }
  CHECK(true);
}

/* --- directed cases --------------------------------------------------- */

/* P2: a spec naming 70 is held to hard_max, then the child gives up. */
static void test_cap(void) {
  static sut s;
  sut_init(&s, true);
  /* value 5 names budget_choices[5] = 70 for child 0, 0 for the others. */
  sut_spec_commit(&s, spec_writer_step_count, 5);
  unsigned int resumes = 0;
  root_restart_action last = root_restart_resume;
  for (unsigned int i = 0; i < hard_max + 4; i++) {
    last = sut_decide(&s, 0);
    resumes += last == root_restart_resume;
  }
  CHECK_EQ_U64(resumes, hard_max);
  CHECK(s.records[0].state == root_child_gone);
  CHECK(last == root_restart_ignore_gone);
}

/* P1 with nothing mapped: the legacy budget exactly, for every child. */
static void test_unmapped_identity(void) {
  static sut s;
  sut_init(&s, false);
  for (unsigned int c = 0; c < chryso_child_count; c++) {
    if (!is_present(c)) {
      continue;
    }
    for (unsigned int i = 0; i < compiled_of(c); i++) {
      CHECK(sut_decide(&s, c) == root_restart_resume);
    }
    CHECK(sut_decide(&s, c) == root_restart_giveup_budget_exhausted);
    CHECK(sut_decide(&s, c) == root_restart_ignore_gone);
    CHECK_EQ_U64(s.records[c].lifetime_count, compiled_of(c));
  }
}

/* The timed selector: a lost clock falls back to the lifetime ceiling and
 * a broken policy reports policy_invalid with a give-up. */
static void test_timed_selector(void) {
  root_spec_state spec = {};
  root_spec_init(&spec);
  const root_child_record fresh = {};
  root_restart_inputs in = {
      .child = 0,
      .compiled = 5,
      .hard_max = hard_max,
      .entry = entry,
      .timed = {.enabled = true,
                .policy = {.capacity = 1,
                           .lifetime_limit = 3,
                           .leak_interval_ticks = 100},
                .lifetime_limit = 3},
      .clock_available = true,
      .now_ticks = 10,
  };
  root_restart_plan_result r = root_restart_plan(&spec, &fresh, &in);
  CHECK(r.action == root_restart_resume);
  CHECK_EQ_U64(r.next.window_count, 1);
  CHECK_EQ_U64(r.next.lifetime_count, 1);
  CHECK_EQ_U64(root_restart_ceiling(&spec, &in), 3);

  /* A full window gives up on the window, not the lifetime. */
  const root_restart_plan_result again = root_restart_plan(&spec, &r.next, &in);
  CHECK(again.action == root_restart_giveup_window_exhausted);
  CHECK(again.next.state == root_child_gone);

  in.clock_available = false;
  root_child_record spent = {.lifetime_count = 3};
  CHECK(root_restart_plan(&spec, &spent, &in).action ==
        root_restart_giveup_budget_exhausted);

  in.clock_available = true;
  root_child_record broken = {.window_count = 1};
  r = root_restart_plan(&spec, &broken, &in);
  CHECK(r.policy_invalid);
  CHECK(r.action == root_restart_giveup_budget_exhausted);

  /* No entry: give up whatever the budget. */
  in.timed.enabled = false;
  in.entry = 0;
  CHECK(root_restart_plan(&spec, &fresh, &in).action ==
        root_restart_giveup_no_entry);

  /* An undeclared state is handled as gone. */
  in.entry = entry;
  const root_child_record odd = {.state = (root_child_state)7};
  r = root_restart_plan(&spec, &odd, &in);
  CHECK(r.next.state == root_child_gone);
}

int main(void) {
  test_cap();
  test_unmapped_identity();
  test_timed_selector();

  cover = (coverage){};
  run_property("P1 identity", model_holds, UINT64_C(0x6a09e667f3bcc908), true,
               true);
  run_property("P1 identity unmapped", model_holds,
               UINT64_C(0x3c6ef372fe94f82b), false, false);
  run_property("spec model", model_holds, UINT64_C(0xbb67ae8584caa73b), false,
               true);
  run_property("P4 substitution", substitution_holds,
               UINT64_C(0xa54ff53a5f1d36f1), false, true);

  /* Every semantic class must have been reached. */
  CHECK(cover.giveups != 0);
  CHECK(cover.applied != 0);
  CHECK(cover.rate_limited != 0);
  CHECK(cover.stale != 0);
  CHECK(cover.already_done != 0);
  CHECK(cover.accepted_restarts != 0);
  CHECK(cover.ignored != 0);
  printf("root_fallback: coverage giveups=%u applied=%u rate_limited=%u "
         "stale=%u already_done=%u accepted=%u ignored=%u\n",
         cover.giveups, cover.applied, cover.rate_limited, cover.stale,
         cover.already_done, cover.accepted_restarts, cover.ignored);
  return check_finish("root_fallback");
}
