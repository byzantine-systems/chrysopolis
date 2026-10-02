/* Bounded copy and validation of Root's one-writer page. The semantic rules
 * (flags, unset rows, event payloads) live in the generated checker, so this
 * reader, the Erlang codec and the golden vectors agree on every verdict. A
 * validated copy can be old; consumers must not read age as a heartbeat.
 *
 * The payload memcpy is a plain read of memory Root writes. It is not a data
 * race only because the one-core image never runs Root and the BEAM at once:
 * Root can preempt a copy, never interleave with it on another core. The
 * sequence bracket then rejects any copy a Root callback overlapped. */
#include "root_status_reader.h"

#include <stdatomic.h>
#include <string.h>

static_assert(sizeof(chryso_root_status_page) == 16384);

[[nodiscard]] bool root_status_copy_begin(const chryso_root_status_page *shared,
                                          chryso_root_status_page *scratch,
                                          uint64_t *seq) {
  const uint64_t before =
      atomic_load_explicit(&shared->header.seq, memory_order_acquire);
  if (before == 0 || (before & 1u) != 0) {
    return false;
  }
  memcpy(scratch, shared, sizeof(*scratch));
  *seq = before;
  return true;
}

[[nodiscard]] root_status_read_result
root_status_copy_finish(const chryso_root_status_page *shared,
                        const chryso_root_status_page *scratch,
                        chryso_root_status_page *output, uint64_t seq) {
  atomic_thread_fence(memory_order_acquire);
  const uint64_t after =
      atomic_load_explicit(&shared->header.seq, memory_order_relaxed);
  if (after != seq) {
    return root_status_read_torn;
  }
  /* The copied seq must match the bracketing reads too. */
  if (atomic_load_explicit(&scratch->header.seq, memory_order_relaxed) != seq ||
      chryso_check_root_status_page((const uint8_t *)scratch,
                                    sizeof(*scratch)) != chryso_abi_reject_ok) {
    return root_status_read_invalid;
  }
  memcpy(output, scratch, sizeof(*output));
  return root_status_read_ok;
}

[[nodiscard]] root_status_read_result root_status_read(
    const chryso_root_status_page *shared, chryso_root_status_page *scratch,
    chryso_root_status_page *output, size_t len, unsigned int attempts) {
  if (shared == nullptr || scratch == nullptr || output == nullptr ||
      scratch == output || scratch == shared || output == shared ||
      len != sizeof(*scratch)) {
    return root_status_read_size;
  }
  for (unsigned int i = 0; i < attempts; i++) {
    uint64_t seq = 0;
    if (!root_status_copy_begin(shared, scratch, &seq)) {
      continue;
    }
    const root_status_read_result result =
        root_status_copy_finish(shared, scratch, output, seq);
    if (result != root_status_read_torn) {
      return result;
    }
  }
  return root_status_read_torn;
}

[[nodiscard]] uint64_t root_status_missed(uint64_t previous_head,
                                          uint64_t current_head) {
  const uint64_t earliest =
      current_head > chryso_event_count ? current_head - chryso_event_count : 0;
  return earliest > previous_head ? earliest - previous_head : 0;
}

static bool root_status_regressed(const chryso_root_status_page *prior,
                                  const chryso_root_status_page *current,
                                  uint64_t prior_seq, uint64_t current_seq) {
  if (current->header.root_generation < prior->header.root_generation) {
    return true;
  }
  if (current->header.root_generation != prior->header.root_generation) {
    return false;
  }
  if (current_seq == prior_seq) {
    /* Root bumps seq for every change, so equal seq means equal bytes. */
    return memcmp(prior, current, sizeof(*current)) != 0;
  }
  if (current_seq < prior_seq ||
      current->header.event_head < prior->header.event_head ||
      current->header.beam_incarnation < prior->header.beam_incarnation) {
    return true;
  }
  for (size_t i = 0; i < chryso_child_count; i++) {
    if (current->children[i].lifetime_count <
            prior->children[i].lifetime_count ||
        current->children[i].cumulative_down_ticks <
            prior->children[i].cumulative_down_ticks) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] root_status_observe_result
root_status_observe(const chryso_root_status_page *prior,
                    const chryso_root_status_page *current,
                    uint64_t reader_ticks, uint64_t reader_frequency,
                    root_status_freshness *out) {
  if (current == nullptr || out == nullptr) {
    return root_status_observe_regressed;
  }
  const uint64_t current_seq =
      atomic_load_explicit(&current->header.seq, memory_order_relaxed);
  const uint64_t prior_seq =
      prior != nullptr
          ? atomic_load_explicit(&prior->header.seq, memory_order_relaxed)
          : 0;
  if (prior != nullptr &&
      root_status_regressed(prior, current, prior_seq, current_seq)) {
    return root_status_observe_regressed;
  }
  const bool same_generation =
      prior != nullptr &&
      current->header.root_generation == prior->header.root_generation;
  const bool age_known = current->header.cntfrq != 0 &&
                         current->header.cntfrq == reader_frequency &&
                         reader_ticks >= current->header.now_ticks;
  *out = (root_status_freshness){
      .missed_events = same_generation
                           ? root_status_missed(prior->header.event_head,
                                                current->header.event_head)
                           : 0,
      .age_ticks = age_known ? reader_ticks - current->header.now_ticks : 0,
      .age_known = age_known,
      .changed = !same_generation || current_seq != prior_seq,
  };
  return root_status_observe_ok;
}
