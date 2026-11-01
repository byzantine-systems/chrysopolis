/*
 * fallback_peer.c - test-only PD that checks driver recovery with the BEAM
 * gone (orch-fallback-smoke, fallback-image only).
 *
 * Root signals the peer on the BEAM give-up channel when it stops beam_server
 * for good. The peer then asks Root, over the timer's fault-injection channel,
 * to fault the timer driver, twice, and reads Root's status page after each to
 * report what Root decided. The peer runs at priority 0: when microkit_notify
 * returns, Root (254) and the timer driver (101) have already handled the
 * injection, the fault and the restart, so each read sees a finished
 * decision. Nothing here spins; the whole run is one notified() call.
 *
 * Root needs no test hook for this. Its channel ids and code paths are the
 * production ones; only the SDF moves the far end of one channel from
 * beam_server to this PD.
 *
 * Machine (any other cell keeps the phase and reports unexpected):
 *
 *   phase \ event  | beam_gone        | restarted(k)        | not_restarted
 *   waiting        | inject, armed_1  | -                   | -
 *   armed_1        | -                | inject, armed_2     | failed
 *   armed_2        | -                | report done, done   | failed
 *   done, failed   | ignore           | ignore              | ignore
 */
#include "root_status_reader.h"

#include <chrysopolis/root_control.h>
#include <chrysopolis/system_abi.h>
#include <runtime_abi.h>

#include <microkit.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Patched by the Microkit tool from the SDF map; 0 if unmapped. */
extern uintptr_t root_status_view;
uintptr_t root_status_view;

static_assert(chryso_control_status_beam_vaddr %
                  alignof(chryso_root_status_page) ==
              0);

typedef enum : uint8_t {
  peer_waiting,
  peer_armed_1,
  peer_armed_2,
  peer_done,
  peer_failed,
  peer_phase_count,
} peer_phase;

typedef enum : uint8_t {
  peer_beam_gone,
  peer_restarted,
  peer_not_restarted,
  peer_event_count,
} peer_event;

typedef enum : uint8_t {
  peer_ignore,
  peer_inject,
  peer_finish,
  peer_fail,
} peer_action;

typedef struct {
  peer_phase next;
  peer_action action;
} peer_cell;

/* The header's table as data. Unlisted cells are designated explicitly so a
 * missing one cannot default to "waiting, ignore" unnoticed. */
static constexpr peer_cell peer_table[peer_phase_count][peer_event_count] = {
    [peer_waiting] =
        {
            [peer_beam_gone] = {peer_armed_1, peer_inject},
            [peer_restarted] = {peer_failed, peer_fail},
            [peer_not_restarted] = {peer_failed, peer_fail},
        },
    [peer_armed_1] =
        {
            [peer_beam_gone] = {peer_failed, peer_fail},
            [peer_restarted] = {peer_armed_2, peer_inject},
            [peer_not_restarted] = {peer_failed, peer_fail},
        },
    [peer_armed_2] =
        {
            [peer_beam_gone] = {peer_failed, peer_fail},
            [peer_restarted] = {peer_done, peer_finish},
            [peer_not_restarted] = {peer_failed, peer_fail},
        },
    [peer_done] =
        {
            [peer_beam_gone] = {peer_done, peer_ignore},
            [peer_restarted] = {peer_done, peer_ignore},
            [peer_not_restarted] = {peer_done, peer_ignore},
        },
    [peer_failed] =
        {
            [peer_beam_gone] = {peer_failed, peer_ignore},
            [peer_restarted] = {peer_failed, peer_ignore},
            [peer_not_restarted] = {peer_failed, peer_ignore},
        },
};

static peer_phase phase = peer_waiting;
static uint32_t timer_count;
static chryso_root_status_page scratch;
static chryso_root_status_page snapshot;

static void put_dec(uint64_t v) {
  char buf[21];
  size_t i = sizeof(buf);
  buf[--i] = '\0';
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0);
  microkit_dbg_puts(&buf[i]);
}

/* A validated copy of Root's page, or false (unmapped, torn or invalid). */
static bool read_status(void) {
  if (root_status_view != chryso_control_status_beam_vaddr) {
    return false;
  }
  return root_status_read((const chryso_root_status_page *)root_status_view,
                          &scratch, &snapshot, sizeof(snapshot),
                          8) == root_status_read_ok;
}

/* restarted when the timer row shows one more restart than last seen, is
 * live, and the newest event is Root's restart of it at that count. */
static peer_event observe_timer(void) {
  if (!read_status()) {
    return peer_not_restarted;
  }
  const chryso_root_child_status *row = &snapshot.children[ROOT_CHILD_TIMER];
  const uint64_t head = snapshot.header.event_head;
  if (head == 0) {
    return peer_not_restarted;
  }
  const chryso_root_event *last =
      &snapshot.events[(head - 1) % chryso_event_count];
  const bool restarted = row->state == chryso_root_child_wire_state_live &&
                         row->lifetime_count == timer_count + 1 &&
                         last->kind == chryso_root_event_kind_restart &&
                         last->child == ROOT_CHILD_TIMER &&
                         last->a == row->lifetime_count;
  if (restarted) {
    timer_count = row->lifetime_count;
  }
  return restarted ? peer_restarted : peer_not_restarted;
}

/* beam_gone only when the BEAM row really reads gone; the baseline timer
 * count comes from the same copy. */
static bool beam_is_gone(void) {
  if (!read_status() || snapshot.children[ROOT_CHILD_BEAM].state !=
                            chryso_root_child_wire_state_gone) {
    return false;
  }
  timer_count = snapshot.children[ROOT_CHILD_TIMER].lifetime_count;
  return true;
}

/* Applies one event and performs its action. Returns the follow-up event an
 * injection produced, or no event (peer_event_count). */
static peer_event step(peer_event event) {
  const peer_cell cell = peer_table[phase][event];
  phase = cell.next;
  switch (cell.action) {
  case peer_inject:
    microkit_notify(FALLBACK_PEER_TIMER_FAULT_CH);
    {
      const peer_event seen = observe_timer();
      if (seen == peer_restarted) {
        microkit_dbg_puts("PEER|fallback|timer|count=");
        put_dec(timer_count);
        microkit_dbg_puts("\n");
      }
      return seen;
    }
  case peer_finish:
    microkit_dbg_puts("PEER|done\n");
    return peer_event_count;
  case peer_fail:
    microkit_dbg_puts("PEER|failed|phase=");
    put_dec(phase);
    microkit_dbg_puts("\n");
    return peer_event_count;
  case peer_ignore:
    break;
  }
  return peer_event_count;
}

void init(void) {
  if (root_status_view != chryso_control_status_beam_vaddr) {
    microkit_dbg_puts("PEER|invalid-map\n");
  }
}

void notified(microkit_channel ch) {
  if (ch != FALLBACK_PEER_BEAM_GONE_CH) {
    microkit_dbg_puts("PEER|unexpected-channel\n");
    return;
  }
  if (!beam_is_gone()) {
    microkit_dbg_puts("PEER|beam-not-gone\n");
    return;
  }
  /* At most three steps: beam_gone, then one per injection. */
  peer_event event = peer_beam_gone;
  for (size_t i = 0; i < 3 && event != peer_event_count; i++) {
    event = step(event);
  }
}
