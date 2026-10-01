/* Root's status publication is a bounded side effect of completed callbacks.
 * Policy-owned counters are projected from the caller's record, never copied
 * into the observation shadow. The one-core image serializes Root callbacks
 * and BEAM reads; atomic sequence stores do not make plain payload accesses
 * safe on an SMP image. */
#include "status.h"

#include <limits.h>
#include <stdatomic.h>

static_assert(sizeof(chryso_root_status_page) == 16384);
static_assert(offsetof(chryso_root_status_page, header.seq) % 8 == 0);
static_assert(chryso_child_count <= 64, "presence is a 64-bit mask");

static bool present(const root_status_state *state, unsigned int child) {
  return child < chryso_child_count &&
         (state->present_mask & (UINT64_C(1) << child)) != 0;
}

static uint16_t fault_word_flags(size_t count) {
  return (uint16_t)((count >= 1 ? chryso_fault_mr0_valid : 0u) |
                    (count >= 2 ? chryso_fault_mr1_valid : 0u));
}

/* Freestanding Root cannot depend on libc memset. The sequence word is
 * skipped: it is only ever accessed atomically. No reader runs while Root
 * initializes the page on the supported one-core image. */
static void clear_page(chryso_root_status_page *page) {
  constexpr size_t seq_begin = offsetof(chryso_root_status_page, header.seq);
  constexpr size_t seq_end = seq_begin + sizeof(page->header.seq);
  volatile uint8_t *bytes = (volatile uint8_t *)page;
  for (size_t i = 0; i < sizeof(*page); i++) {
    if (i < seq_begin || i >= seq_end) {
      bytes[i] = 0;
    }
  }
}

void root_status_init(root_status_state *state, chryso_root_status_page *page,
                      uint64_t present_mask, uint64_t now, uint64_t frequency,
                      uint32_t driver_budget, uint32_t beam_budget,
                      unsigned int beam_id) {
  *state = (root_status_state){};
  state->present_mask = present_mask;
  state->root_generation = 1; /* Root is not restartable in this topology. */
  state->beam_incarnation = 1;
  state->frequency = frequency;
  if (page == nullptr) {
    return;
  }
  const uint64_t stamp = frequency != 0 ? now : 0;
  atomic_store_explicit(&page->header.seq, 0, memory_order_relaxed);
  clear_page(page);
  page->header.magic = chryso_magic_status;
  page->header.abi_version = chryso_abi_version;
  page->header.child_count = chryso_child_count;
  page->header.root_generation = state->root_generation;
  page->header.beam_incarnation = state->beam_incarnation;
  page->header.now_ticks = stamp;
  page->header.cntfrq = frequency;
  for (unsigned int i = 0; i < chryso_child_count; i++) {
    if (!present(state, i)) {
      continue;
    }
    page->children[i].state = chryso_root_child_wire_state_live;
    page->children[i].desired = chryso_root_desired_running;
    page->children[i].effective_budget =
        i == beam_id ? beam_budget : driver_budget;
    page->children[i].boot_ticks = stamp;
    state->observations[i].boot_ticks = stamp;
  }
  page->events[0] = (chryso_root_event){
      .ticks = stamp,
      .kind = chryso_root_event_kind_boot,
      .child = chryso_root_event_no_child,
      .a = state->root_generation,
      .b = state->beam_incarnation,
  };
  state->event_head = 1;
  page->header.event_head = 1;
  atomic_store_explicit(&page->header.seq, 2, memory_order_release);
  state->seq = 2;
  state->enabled = true;
}

void root_status_note_fault(root_status_state *state, unsigned int child,
                            uint64_t label, size_t count, uint64_t mr0,
                            uint64_t mr1, uint64_t now, bool clock_valid) {
  if (!present(state, child)) {
    return;
  }
  root_status_observation *obs = &state->observations[child];
  /* The wire field is 32-bit; zero records an unrepresentable raw label.
   * Fault policy still receives and logs the original seL4 word. */
  obs->fault_label = label <= UINT32_MAX ? (uint32_t)label : 0;
  obs->fault_flags = fault_word_flags(count);
  obs->fault_mr0 = count >= 1 ? mr0 : 0;
  obs->fault_mr1 = count >= 2 ? mr1 : 0;
  obs->last_fault_ticks = clock_valid ? now : 0;
  if (clock_valid && !obs->down_open) {
    obs->down_since_ticks = now;
    obs->down_open = true;
  }
}

void root_status_note_restart(root_status_state *state, unsigned int child,
                              uint64_t now, bool clock_valid,
                              bool beam_restart) {
  if (!present(state, child)) {
    return;
  }
  root_status_observation *obs = &state->observations[child];
  obs->last_restart_ticks = clock_valid ? now : 0;
  if (obs->down_open) {
    /* A counter that went backwards closes the interval without adding. */
    if (clock_valid && now >= obs->down_since_ticks) {
      const uint64_t elapsed = now - obs->down_since_ticks;
      obs->cumulative_down_ticks =
          elapsed > UINT64_MAX - obs->cumulative_down_ticks
              ? UINT64_MAX
              : obs->cumulative_down_ticks + elapsed;
    }
    obs->down_open = false;
  }
  if (beam_restart) {
    if (state->beam_incarnation == UINT64_MAX) {
      state->enabled = false;
      state->exhausted = true;
    } else {
      state->beam_incarnation++;
    }
  }
}

void root_status_note_gone(root_status_state *state, unsigned int child,
                           uint64_t now, bool clock_valid) {
  if (!present(state, child)) {
    return;
  }
  root_status_observation *obs = &state->observations[child];
  if (clock_valid && !obs->down_open) {
    obs->down_since_ticks = now;
    obs->down_open = true;
  }
}

[[nodiscard]] bool
root_status_publish(root_status_state *state, chryso_root_status_page *page,
                    unsigned int child, const root_child_record *record,
                    uint32_t effective_budget, uint64_t now, bool clock_valid,
                    const chryso_root_event *events, size_t event_count) {
  if (!state->enabled || page == nullptr) {
    return false;
  }
  if (event_count > 2 || (event_count != 0 && events == nullptr) ||
      (record != nullptr && !present(state, child))) {
    state->enabled = false;
    state->misused = true;
    return false;
  }
  if (state->seq > UINT64_MAX - 2 ||
      event_count > UINT64_MAX - state->event_head) {
    state->enabled = false;
    state->exhausted = true;
    return false;
  }
  /* Odd, payload, even. The fence and release store order the plain payload
   * stores only as far as the compiler is concerned; the reader is excluded
   * by the one-core image, not by these atomics. */
  atomic_store_explicit(&page->header.seq, state->seq + 1,
                        memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  if (record != nullptr) {
    const root_status_observation *obs = &state->observations[child];
    chryso_root_child_status *row = &page->children[child];
    row->state = record->state == root_child_gone
                     ? chryso_root_child_wire_state_gone
                     : chryso_root_child_wire_state_live;
    row->desired = chryso_root_desired_running;
    row->flags = (uint16_t)(obs->fault_flags |
                            (obs->down_open ? chryso_down_interval_open : 0u));
    row->lifetime_count = record->lifetime_count;
    row->effective_budget = effective_budget;
    row->fault_label = obs->fault_label;
    row->fault_mr0 = obs->fault_mr0;
    row->fault_mr1 = obs->fault_mr1;
    row->last_fault_ticks = obs->last_fault_ticks;
    row->last_restart_ticks = obs->last_restart_ticks;
    row->cumulative_down_ticks = obs->cumulative_down_ticks;
    row->window_count = record->window_count;
  }
  for (size_t i = 0; i < event_count; i++) {
    page->events[state->event_head % chryso_event_count] = events[i];
    state->event_head++;
  }
  page->header.event_head = state->event_head;
  page->header.event_dropped = state->event_head > chryso_event_count
                                   ? state->event_head - chryso_event_count
                                   : 0;
  page->header.beam_incarnation = state->beam_incarnation;
  page->header.now_ticks = clock_valid ? now : 0;
  page->header.cntfrq = clock_valid ? state->frequency : 0;
  atomic_store_explicit(&page->header.seq, state->seq + 2,
                        memory_order_release);
  state->seq += 2;
  return true;
}
