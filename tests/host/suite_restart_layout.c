/* beam_restart_layout.h: range arithmetic and the layout gate that runs
 * before a capture or restore touches memory. */
#include "check.h"

#include "beam_restart_layout.h"

#include <stdint.h>
#include <string.h>

/* The shape of the real image, at smaller sizes: the writable segment low,
 * the snapshot region above it, the exit-fault page above both. */
static beam_restart_layout sample(void) {
  return (beam_restart_layout){
      .snapshot_base = 0x30000000,
      .snapshot_size = 0x80000,
      .data_off = 0x6000,
      .segment_start = 0x400000,
      .bss_start = 0x430000,
      .bss_end = 0x2000000,
      .exit_fault_base = 0xbe9ff000,
      .exit_fault_size = 0x1000,
  };
}

static void test_range_helpers(void) {
  CHECK(beam_range_fits(0, 0));
  CHECK(beam_range_fits(UINTPTR_MAX, 0));
  CHECK(beam_range_fits(UINTPTR_MAX - 9, 9));
  CHECK(!beam_range_fits(UINTPTR_MAX - 9, 10));
  CHECK(!beam_range_fits(1, SIZE_MAX));
  /* An end of exactly 2^N is not representable, so it counts as a wrap. */
  CHECK(!beam_range_fits(UINTPTR_MAX - 0xfff, 0x1000));

  /* Touching is not overlapping; one shared byte is. */
  CHECK(!beam_ranges_overlap(0x1000, 0x1000, 0x2000, 0x1000));
  CHECK(!beam_ranges_overlap(0x2000, 0x1000, 0x1000, 0x1000));
  CHECK(beam_ranges_overlap(0x1000, 0x1001, 0x2000, 0x1000));
  CHECK(beam_ranges_overlap(0x2000, 0x1000, 0x1000, 0x1001));
  /* Containment either way. */
  CHECK(beam_ranges_overlap(0x1000, 0x10000, 0x2000, 0x10));
  CHECK(beam_ranges_overlap(0x2000, 0x10, 0x1000, 0x10000));
  /* Empty ranges overlap nothing, even inside another range. */
  CHECK(!beam_ranges_overlap(0x2000, 0, 0x1000, 0x10000));
  CHECK(!beam_ranges_overlap(0x1000, 0x10000, 0x2000, 0));
  /* A range ending at the highest representable end. */
  CHECK(beam_ranges_overlap(UINTPTR_MAX - 0x1000, 0x1000, UINTPTR_MAX - 1, 1));
  CHECK(!beam_ranges_overlap(UINTPTR_MAX - 0x1000, 0x1000, UINTPTR_MAX, 0));
  CHECK(!beam_ranges_overlap(UINTPTR_MAX - 0x1000, 0x1000, UINTPTR_MAX - 1, 0));

  /* A stack top at the start is outside, at the end is inside. */
  CHECK(!beam_range_holds_stack_top(0x1000, 0x1000, 0x1000));
  CHECK(beam_range_holds_stack_top(0x1000, 0x1000, 0x1001));
  CHECK(beam_range_holds_stack_top(0x1000, 0x1000, 0x2000));
  CHECK(!beam_range_holds_stack_top(0x1000, 0x1000, 0x2001));
  CHECK(!beam_range_holds_stack_top(0x1000, 0x1000, 0));
  CHECK(!beam_range_holds_stack_top(0x1000, 0, 0x1000));
  CHECK(beam_range_holds_stack_top(UINTPTR_MAX - 0x1000, 0x1000, UINTPTR_MAX));
}

static void test_sample_is_ok(void) {
  const beam_restart_layout layout = sample();
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
}

static void test_zero_base(void) {
  beam_restart_layout layout = sample();
  layout.snapshot_base = 0;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_zero_base);
}

static void test_wraps(void) {
  beam_restart_layout layout = sample();
  layout.snapshot_base = UINTPTR_MAX - layout.snapshot_size;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.snapshot_base++;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_range_wraps);

  layout = sample();
  layout.exit_fault_base = UINTPTR_MAX - layout.exit_fault_size;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.exit_fault_base++;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_range_wraps);
}

static void test_segment_order(void) {
  beam_restart_layout layout = sample();
  layout.segment_start = layout.bss_start + 1;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_segment_order);

  layout = sample();
  layout.bss_end = layout.bss_start - 1;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_segment_order);

  /* Empty data and empty .bss are both in order. */
  layout = sample();
  layout.segment_start = layout.bss_start;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.bss_end = layout.bss_start;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
}

static void test_bss_too_large(void) {
  beam_restart_layout layout = sample();
  layout.snapshot_base = 0x7f0000000000;
  layout.exit_fault_base = 0x7ff000000000;
  layout.bss_end = layout.bss_start + UINT32_MAX;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.bss_end++;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_bss_too_large);
}

static void test_data_overflow(void) {
  beam_restart_layout layout = sample();
  const size_t capacity = layout.snapshot_size - layout.data_off;
  layout.bss_start = layout.segment_start + capacity;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.bss_start++;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_data_overflow);

  /* A data offset past the region leaves no capacity to subtract from. */
  layout = sample();
  layout.data_off = layout.snapshot_size;
  layout.segment_start = layout.bss_start;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.data_off++;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_data_overflow);
}

static void test_snapshot_overlaps_segment(void) {
  beam_restart_layout layout = sample();
  /* The snapshot ends exactly where the segment starts: fine. */
  layout.snapshot_base = layout.segment_start - layout.snapshot_size;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.snapshot_base++;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_snapshot_overlaps_segment);

  /* The snapshot starts exactly at the end of .bss: fine. */
  layout = sample();
  layout.snapshot_base = layout.bss_end;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.snapshot_base--;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_snapshot_overlaps_segment);

  /* Inside .data rather than .bss is just as bad. */
  layout = sample();
  layout.snapshot_base = layout.segment_start + 8;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_snapshot_overlaps_segment);
}

static void test_exit_fault_overlaps(void) {
  beam_restart_layout layout = sample();
  layout.exit_fault_base = layout.snapshot_base + layout.snapshot_size;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.exit_fault_base--;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_exit_fault_overlaps);

  layout = sample();
  layout.exit_fault_base = layout.segment_start - layout.exit_fault_size;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
  layout.exit_fault_base++;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_exit_fault_overlaps);

  layout = sample();
  layout.exit_fault_base = layout.bss_end - 1;
  CHECK(beam_restart_layout_check(&layout) ==
        beam_restart_layout_exit_fault_overlaps);

  /* A zero-size exit range can never be mapped by anything. */
  layout.exit_fault_size = 0;
  CHECK(beam_restart_layout_check(&layout) == beam_restart_layout_ok);
}

static void test_status_names(void) {
  static const beam_restart_layout_status all[] = {
      beam_restart_layout_ok,
      beam_restart_layout_zero_base,
      beam_restart_layout_range_wraps,
      beam_restart_layout_segment_order,
      beam_restart_layout_bss_too_large,
      beam_restart_layout_data_overflow,
      beam_restart_layout_snapshot_overlaps_segment,
      beam_restart_layout_exit_fault_overlaps,
  };
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    const char *name = beam_restart_layout_status_name(all[i]);
    CHECK(name[0] != '\0');
    CHECK(strcmp(name, "unknown") != 0);
    for (size_t j = 0; j < i; j++) {
      CHECK(strcmp(name, beam_restart_layout_status_name(all[j])) != 0);
    }
  }
}

int main(void) {
  test_range_helpers();
  test_sample_is_ok();
  test_zero_base();
  test_wraps();
  test_segment_order();
  test_bss_too_large();
  test_data_overflow();
  test_snapshot_overlaps_segment();
  test_exit_fault_overlaps();
  test_status_names();
  return check_finish("restart_layout");
}
