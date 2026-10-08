/* Test-image-only restart-during-commit fixture for Root's spec consumer.
 * Off unless modules/images.nix patches the magic into .spec_fixture_config,
 * so the shared ERTS test ELF behaves as before in every other image.
 *
 * Incarnation 1 commits generation 1 (BEAM budget 1) and leaves generation 2
 * half written, so the next BEAM fault reaches Root mid-commit. Incarnation 2
 * checks Root applied generation 1, then commits a conflicting generation 1
 * (BEAM budget 64). Root must reject it, keep budget 1 and give up on the
 * next fault, where the compiled budget alone would restart. The page
 * survives the BEAM reset because it is an MR outside the restored range. */
#include "root_status_reader.h"
#include "spec_writer.h"

#include <chrysopolis/system_abi.h>
#include <runtime_abi.h>

#include <microkit.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "SPECFIX1" little-endian; the image build writes it to enable the fixture. */
static constexpr uint64_t spec_fixture_magic = UINT64_C(0x3158494643455053);

__attribute__((__section__(".spec_fixture_config"),
               used)) volatile uint64_t spec_fixture_config = 0;

extern uintptr_t root_status_view;
extern uintptr_t orchestrator_spec_start;

static chryso_root_status_page fixture_scratch;
static chryso_root_status_page fixture_status;
static spec_writer fixture_writer;
static uint32_t fixture_budget[chryso_child_count];
static uint8_t fixture_desired[chryso_child_count];

void spec_fixture_run(void);

static void fixture_policy(uint32_t beam_budget) {
  for (size_t c = 0; c < chryso_child_count; c++) {
    fixture_budget[c] = c == ROOT_CHILD_BEAM ? beam_budget : 0;
    fixture_desired[c] = 0;
  }
}

/* Incarnation 1: commit generation 1, then stop generation 2 after write. */
static const char *fixture_open(chryso_spec_page *page) {
  fixture_policy(1);
  spec_writer_init(&fixture_writer);
  if (spec_writer_commit(&fixture_writer, page, 1, fixture_budget,
                         fixture_desired) != spec_write_ok) {
    return "SPEC_FIXTURE|writer-refused\n";
  }
  fixture_policy(64);
  if (spec_writer_begin(&fixture_writer, page, 2, fixture_budget,
                        fixture_desired) != spec_write_ok ||
      spec_writer_step(&fixture_writer, spec_writer_step_open) !=
          spec_write_ok ||
      spec_writer_step(&fixture_writer, spec_writer_step_write) !=
          spec_write_ok) {
    return "SPEC_FIXTURE|writer-refused\n";
  }
  return "SPEC_FIXTURE|open|committed=1|pending=2\n";
}

/* Incarnation 2: Root applied generation 1 and the half-written bank is
 * still odd; then stage a conflicting generation 1. */
static const char *fixture_conflict(chryso_spec_page *page) {
  const uint32_t applied = atomic_load(&page->header.active_bank);
  const uint32_t pending = 1 - applied;
  const chryso_root_child_status *beam =
      &fixture_status.children[ROOT_CHILD_BEAM];
  if (fixture_status.header.applied_spec_generation != 1 ||
      fixture_status.header.applied_bank != applied ||
      beam->effective_budget != 1 ||
      (atomic_load(&page->banks[pending].bank_seq) & 1u) == 0) {
    return "SPEC_FIXTURE|unexpected-applied\n";
  }
  microkit_dbg_puts("SPEC_FIXTURE|applied|generation=1\n");
  fixture_policy(64);
  spec_writer_init(&fixture_writer);
  if (spec_writer_commit(&fixture_writer, page, 1, fixture_budget,
                         fixture_desired) != spec_write_ok) {
    return "SPEC_FIXTURE|writer-refused\n";
  }
  return "SPEC_FIXTURE|conflict-staged\n";
}

void spec_fixture_run(void) {
  if (spec_fixture_config != spec_fixture_magic) {
    return;
  }
  if (root_status_view != chryso_control_status_beam_vaddr ||
      orchestrator_spec_start != chryso_control_spec_beam_vaddr) {
    microkit_dbg_puts("SPEC_FIXTURE|invalid-map\n");
    return;
  }
  if (root_status_read((const chryso_root_status_page *)root_status_view,
                       &fixture_scratch, &fixture_status,
                       sizeof(fixture_status), 3) != root_status_read_ok) {
    microkit_dbg_puts("SPEC_FIXTURE|status-unreadable\n");
    return;
  }
  chryso_spec_page *page = (chryso_spec_page *)orchestrator_spec_start;
  switch (fixture_status.header.beam_incarnation) {
  case 1:
    microkit_dbg_puts(fixture_open(page));
    return;
  case 2:
    microkit_dbg_puts(fixture_conflict(page));
    return;
  default:
    microkit_dbg_puts("SPEC_FIXTURE|later\n");
    return;
  }
}
