/* Test-image-only read of the status mapping through the same reader the host
 * suite proves. No Root PPC or notification is involved: a restarted BEAM
 * relists the same authoritative shared page. */
#include "root_status_reader.h"

#include <chrysopolis/system_abi.h>
#include <runtime_abi.h>

#include <microkit.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern uintptr_t root_status_view;

static chryso_root_status_page scratch;
static chryso_root_status_page snapshot;

static_assert(chryso_control_status_beam_vaddr %
                  alignof(chryso_root_status_page) ==
              0);

void root_status_probe(void);

static const chryso_root_event *root_status_probe_event(uint64_t index) {
  return &snapshot.events[index % chryso_event_count];
}

/* The newest events must be this BEAM's fault and Root's restart of it,
 * carrying the counters its row now shows. Root may record one spec
 * rejection between them, seen while it decided. */
static bool
root_status_probe_restart_journal(const chryso_root_child_status *beam) {
  const uint64_t head = snapshot.header.event_head;
  if (head < 3) {
    return false;
  }
  const bool rejected = head >= 4 && root_status_probe_event(head - 2)->kind ==
                                         chryso_root_event_kind_spec_reject;
  const chryso_root_event *fault =
      root_status_probe_event(head - (rejected ? 3 : 2));
  const chryso_root_event *restart = root_status_probe_event(head - 1);
  return fault->kind == chryso_root_event_kind_fault &&
         fault->child == ROOT_CHILD_BEAM &&
         (fault->flags & chryso_fault_mr0_valid) != 0 &&
         restart->kind == chryso_root_event_kind_restart &&
         restart->child == ROOT_CHILD_BEAM &&
         restart->a == beam->lifetime_count &&
         restart->b == beam->window_count && restart->ticks >= fault->ticks &&
         beam->last_restart_ticks >= beam->last_fault_ticks;
}

static const char *root_status_probe_verdict(void) {
  const chryso_root_child_status *beam = &snapshot.children[ROOT_CHILD_BEAM];
  const uint8_t crasher = snapshot.children[ROOT_CHILD_CRASHER].state;
  const chryso_root_event *boot = root_status_probe_event(0);
  const bool boot_retained = snapshot.header.event_head <= chryso_event_count;
  if (snapshot.header.root_generation != 1 ||
      beam->state != chryso_root_child_wire_state_live ||
      (crasher != chryso_root_child_wire_state_unset &&
       crasher != chryso_root_child_wire_state_live &&
       crasher != chryso_root_child_wire_state_gone) ||
      (boot_retained && (boot->kind != chryso_root_event_kind_boot ||
                         boot->child != chryso_root_event_no_child ||
                         boot->a != 1 || boot->b != 1))) {
    return "ROOT_STATUS|invalid-rows\n";
  }
  const uint64_t incarnation = snapshot.header.beam_incarnation;
  if (incarnation == 1 && beam->lifetime_count == 0) {
    /* Other children may already have events (a restart image's crasher);
     * none may concern a BEAM that has never been restarted. */
    const uint64_t head = snapshot.header.event_head;
    const uint64_t earliest =
        head > chryso_event_count ? head - chryso_event_count : 0;
    for (uint64_t i = earliest; i < head; i++) {
      if (root_status_probe_event(i)->child == ROOT_CHILD_BEAM) {
        return "ROOT_STATUS|unexpected-journal\n";
      }
    }
    return "ROOT_STATUS|boot|incarnation=1\n";
  }
  if (!root_status_probe_restart_journal(beam)) {
    return "ROOT_STATUS|unexpected-journal\n";
  }
  if (incarnation == 2 && beam->lifetime_count == 1) {
    return "ROOT_STATUS|restart|incarnation=2|count=1\n";
  }
  if (incarnation > 2 && beam->lifetime_count == incarnation - 1) {
    return "ROOT_STATUS|restart|later\n";
  }
  return "ROOT_STATUS|unexpected-transition\n";
}

void root_status_probe(void) {
  if (root_status_view != chryso_control_status_beam_vaddr) {
    microkit_dbg_puts("ROOT_STATUS|invalid-map\n");
    return;
  }
  const chryso_root_status_page *const page =
      (const chryso_root_status_page *)root_status_view;
  switch (root_status_read(page, &scratch, &snapshot, sizeof(snapshot), 3)) {
  case root_status_read_ok:
    microkit_dbg_puts(root_status_probe_verdict());
    return;
  case root_status_read_invalid:
    microkit_dbg_puts("ROOT_STATUS|invalid-page\n");
    return;
  case root_status_read_torn:
  case root_status_read_size:
    microkit_dbg_puts("ROOT_STATUS|torn\n");
    return;
  }
}
