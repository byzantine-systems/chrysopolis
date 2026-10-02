#ifndef CHRYSOPOLIS_ROOT_STATUS_H
#define CHRYSOPOLIS_ROOT_STATUS_H 1

/* Writer side of Root's status page. Pure: no globals, allocation, logging or
 * Microkit calls, so Root links it and tests/host runs it. The reader side is
 * src/lib/abi/root_status_reader.h. Not thread-safe: Root's callbacks are
 * serialized. */
#include "root_policy.h"
#include <chrysopolis/root_control.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Policy state stays in root_child_record. These observations are never used
 * to select a fault action, and Root never reads its shared page back. */
typedef struct {
  uint32_t fault_label;
  uint16_t fault_flags;
  uint64_t fault_mr0;
  uint64_t fault_mr1;
  uint64_t last_fault_ticks;
  uint64_t last_restart_ticks;
  uint64_t boot_ticks;
  uint64_t cumulative_down_ticks;
  uint64_t down_since_ticks;
  bool down_open;
} root_status_observation;

typedef struct {
  root_status_observation observations[chryso_child_count];
  uint64_t present_mask;
  uint64_t seq;
  uint64_t event_head;
  uint64_t root_generation;
  uint64_t beam_incarnation;
  uint64_t frequency;
  bool enabled;
  /* Set when publication stops; the adapter reports each once and clears it.
   * exhausted: a counter would wrap. misused: invalid publish arguments. */
  bool exhausted;
  bool misused;
} root_status_state;

/* Resets state and, when page is non-null, writes every byte of the page
 * before publishing seq = 2. A null page leaves publication disabled, which
 * changes nothing about Root's fault policy. present_mask bits at or above
 * chryso_child_count are ignored. state must be non-null. */
void root_status_init(root_status_state *state, chryso_root_status_page *page,
                      uint64_t present_mask, uint64_t now, uint64_t frequency,
                      uint32_t driver_budget, uint32_t beam_budget,
                      unsigned int beam_id);

/* Record observations in state only; nothing reaches the page until
 * root_status_publish. A child that is out of range or absent from
 * present_mask is ignored. state must be non-null. */
void root_status_note_fault(root_status_state *state, unsigned int child,
                            uint64_t label, size_t count, uint64_t mr0,
                            uint64_t mr1, uint64_t now, bool clock_valid);
/* beam_restart advances beam_incarnation; at its maximum, publication stops
 * with exhausted set. */
void root_status_note_restart(root_status_state *state, unsigned int child,
                              uint64_t now, bool clock_valid,
                              bool beam_restart);
void root_status_note_gone(root_status_state *state, unsigned int child,
                           uint64_t now, bool clock_valid);

/* Writes the header, the row for child when record is non-null, and up to two
 * events, between one odd and the next even seq. Returns false and writes
 * nothing when publication is disabled or page is null; on a would-be wrap
 * (exhausted) or an absent child, bad event count or null events (misused) it
 * also disables publication. The last complete page stays readable, and the
 * caller must carry on with its child action whatever this returns. */
[[__nodiscard__]] bool
root_status_publish(root_status_state *state, chryso_root_status_page *page,
                    unsigned int child, const root_child_record *record,
                    uint32_t effective_budget, uint64_t now, bool clock_valid,
                    const chryso_root_event *events, size_t event_count);

#endif
