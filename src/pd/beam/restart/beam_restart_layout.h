#ifndef CHRYSOPOLIS_BEAM_RESTART_LAYOUT_H
#define CHRYSOPOLIS_BEAM_RESTART_LAYOUT_H 1

#include <stdbool.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Address layout checks for beam_server's warm restart. Header-only and pure:
 * the caller supplies the linker symbols, the patched snapshot base and the
 * ABI sizes, and decides what a failure means (restart.c faults to Root).
 * Every range is half-open, [start, start + size), and every end is computed
 * with checked arithmetic, so no address in here can wrap.
 */

typedef struct {
  /* The snapshot region, patched in by the Microkit tool. */
  uintptr_t snapshot_base;
  size_t snapshot_size;
  /* Offset of the data area inside the snapshot region. */
  size_t data_off;
  /* The writable segment: [segment_start, bss_start) is file-backed data,
   * [bss_start, bss_end) is .bss. */
  uintptr_t segment_start;
  uintptr_t bss_start;
  uintptr_t bss_end;
  /* The unmapped range whose store carries an exit code to Root. */
  uintptr_t exit_fault_base;
  size_t exit_fault_size;
} beam_restart_layout;

typedef enum {
  beam_restart_layout_ok,
  /* The snapshot region was never mapped (beam_snapshot_start is 0). */
  beam_restart_layout_zero_base,
  /* The snapshot or exit-fault range runs past the top of the address
   * space. */
  beam_restart_layout_range_wraps,
  /* The linker symbols are not ordered segment_start <= bss_start <=
   * bss_end. */
  beam_restart_layout_segment_order,
  /* .bss is too large for the 32-bit survivor record offsets. */
  beam_restart_layout_bss_too_large,
  /* The file-backed data does not fit the snapshot data area. */
  beam_restart_layout_data_overflow,
  /* Restoring the segment would overwrite the snapshot it restores from. */
  beam_restart_layout_snapshot_overlaps_segment,
  /* The exit-fault range is mapped by the snapshot or the segment, so the
   * restart request would be a plain store. */
  beam_restart_layout_exit_fault_overlaps,
} beam_restart_layout_status;

/* True when [start, start + size) has an end that fits in uintptr_t. */
[[__nodiscard__]] static inline bool beam_range_fits(uintptr_t start,
                                                     size_t size) {
  uintptr_t end = 0;
  return !ckd_add(&end, start, size);
}

/*
 * True when two ranges share at least one byte. Both ranges must satisfy
 * beam_range_fits. An empty range overlaps nothing, and ranges that only
 * touch do not overlap.
 */
[[__nodiscard__]] static inline bool
beam_ranges_overlap(uintptr_t a, size_t a_size, uintptr_t b, size_t b_size) {
  return a_size != 0 && b_size != 0 && a < b + b_size && b < a + a_size;
}

/*
 * True when a descending stack whose top is sp would use memory inside
 * [start, start + size): the bytes a push writes lie just below sp, so a top
 * equal to start is outside and a top equal to the end is inside. The range
 * must satisfy beam_range_fits.
 */
[[__nodiscard__]] static inline bool
beam_range_holds_stack_top(uintptr_t start, size_t size, uintptr_t sp) {
  return sp > start && sp - start <= size;
}

/* Check a layout before either the capture or the restore touches memory.
 * layout must be non-null. */
[[__nodiscard__]] static inline beam_restart_layout_status
beam_restart_layout_check(const beam_restart_layout *layout) {
  if (layout->snapshot_base == 0) {
    return beam_restart_layout_zero_base;
  }
  if (!beam_range_fits(layout->snapshot_base, layout->snapshot_size) ||
      !beam_range_fits(layout->exit_fault_base, layout->exit_fault_size)) {
    return beam_restart_layout_range_wraps;
  }
  if (layout->segment_start > layout->bss_start ||
      layout->bss_start > layout->bss_end) {
    return beam_restart_layout_segment_order;
  }
  if (layout->bss_end - layout->bss_start > UINT32_MAX) {
    return beam_restart_layout_bss_too_large;
  }
  if (layout->data_off > layout->snapshot_size ||
      layout->bss_start - layout->segment_start >
          layout->snapshot_size - layout->data_off) {
    return beam_restart_layout_data_overflow;
  }
  const size_t segment_size = layout->bss_end - layout->segment_start;
  if (beam_ranges_overlap(layout->snapshot_base, layout->snapshot_size,
                          layout->segment_start, segment_size)) {
    return beam_restart_layout_snapshot_overlaps_segment;
  }
  if (beam_ranges_overlap(layout->exit_fault_base, layout->exit_fault_size,
                          layout->snapshot_base, layout->snapshot_size) ||
      beam_ranges_overlap(layout->exit_fault_base, layout->exit_fault_size,
                          layout->segment_start, segment_size)) {
    return beam_restart_layout_exit_fault_overlaps;
  }
  return beam_restart_layout_ok;
}

/* The reason logged for a layout status. */
[[__nodiscard__]] static inline const char *
beam_restart_layout_status_name(beam_restart_layout_status status) {
  switch (status) {
  case beam_restart_layout_ok:
    return "ok";
  case beam_restart_layout_zero_base:
    return "no-snapshot-region";
  case beam_restart_layout_range_wraps:
    return "range-wraps";
  case beam_restart_layout_segment_order:
    return "segment-order";
  case beam_restart_layout_bss_too_large:
    return "bss-too-large";
  case beam_restart_layout_data_overflow:
    return "data-area-overflow";
  case beam_restart_layout_snapshot_overlaps_segment:
    return "snapshot-overlaps-segment";
  case beam_restart_layout_exit_fault_overlaps:
    return "exit-fault-mapped";
  }
  return "unknown";
}

#endif
