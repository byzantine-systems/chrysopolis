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
  /* The policy rows publish: the lifetime ceiling and desired state of each
   * child, and the spec generation and bank they came from. */
  uint32_t effective_budget[chryso_child_count];
  uint8_t desired[chryso_child_count];
  uint64_t applied_generation;
  uint32_t applied_bank;
  /* Set by root_status_note_policy: the next publish rewrites every row. */
  bool policy_dirty;
  bool enabled;
  /* Set when publication stops; the adapter reports each once and clears it.
   * exhausted: a counter would wrap. misused: invalid publish arguments. */
  bool exhausted;
  bool misused;
} root_status_state;

/* Resets state and, when page is non-null, writes every byte of the page
 * before publishing seq = 2. budget[] is each child's compiled lifetime
 * ceiling; every present row starts running with no spec applied. A null page
 * leaves publication disabled, which changes nothing about Root's fault
 * policy. present_mask bits at or above chryso_child_count are ignored.
 * state must be non-null. */
void root_status_init(root_status_state *state, chryso_root_status_page *page,
                      uint64_t present_mask, uint64_t now, uint64_t frequency,
                      const uint32_t budget[static chryso_child_count]);

/* Records the policy Root now decides with. Nothing reaches the page until
 * the next publish, which then rewrites every present row's budget and
 * desired state and the header's applied generation and bank. */
void root_status_note_policy(root_status_state *state, uint64_t generation,
                             uint32_t bank,
                             const uint32_t budget[static chryso_child_count],
                             const uint8_t desired[static chryso_child_count]);

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

/* Writes the header, the row for child when record is non-null, every
 * present row after root_status_note_policy, and up to three events, between
 * one odd and the next even seq. Returns false and writes
 * nothing when publication is disabled or page is null; on a would-be wrap
 * (exhausted) or an absent child, bad event count or null events (misused) it
 * also disables publication. The last complete page stays readable, and the
 * caller must carry on with its child action whatever this returns. */
[[__nodiscard__]] bool
root_status_publish(root_status_state *state, chryso_root_status_page *page,
                    unsigned int child, const root_child_record *record,
                    uint64_t now, bool clock_valid,
                    const chryso_root_event *events, size_t event_count);

#endif
