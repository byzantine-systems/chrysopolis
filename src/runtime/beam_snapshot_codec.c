/*
 * .bss is tens of megabytes and all but a few dozen bytes of it are zero, so
 * the scan loads a word at a time. The load uses __builtin_memcpy because the
 * runtime builds with -ffreestanding, which stops the compiler from treating a
 * plain memcpy call as the inlinable builtin; a real call per word would cost
 * most of a second under TCG. Loading through memcpy rather than a cast also
 * keeps the scan valid for a buffer of any alignment.
 */
#include "beam_snapshot_codec.h"

#include <stdckdint.h>
#include <string.h>

static constexpr size_t word_bytes = sizeof(uint64_t);
static constexpr size_t header_bytes = sizeof(beam_survivor_hdr_t);

static_assert(UINTPTR_MAX == UINT64_MAX,
              "the snapshot header stores addresses as 64-bit words");

static uint64_t load_word(const uint8_t *bss, size_t index) {
  uint64_t word = 0;
  __builtin_memcpy(&word, bss + index * word_bytes, word_bytes);
  return word;
}

/* Append one record, or report that it does not fit. */
static bool append_run(const uint8_t *bss, size_t offset, size_t len,
                       uint8_t *out, size_t capacity, size_t *used) {
  if (capacity - *used < header_bytes ||
      capacity - *used - header_bytes < len) {
    return false;
  }
  /* The caller bounds bss_len by UINT32_MAX, so both narrowings are exact. */
  const beam_survivor_hdr_t hdr = {.offset = (uint32_t)offset,
                                   .len = (uint32_t)len};
  memcpy(out + *used, &hdr, header_bytes);
  memcpy(out + *used + header_bytes, bss + offset, len);
  *used += header_bytes + len;
  return true;
}

beam_snapshot_status beam_survivors_encode(const uint8_t *bss, size_t bss_len,
                                           size_t gap_words, uint8_t *out,
                                           size_t capacity, size_t *used) {
  if (bss_len > UINT32_MAX) {
    return beam_snapshot_bss_too_large;
  }
  const size_t nwords = bss_len / word_bytes;
  size_t written = 0;

  for (size_t i = 0; i < nwords;) {
    if (load_word(bss, i) == 0) {
      i++;
      continue;
    }

    /* Extend the run while the next non-zero word is within the coalescing
     * gap, so one patched struct stays one entry. */
    const size_t start = i;
    size_t last = i;
    i++;
    while (i < nwords && i - last <= gap_words) {
      if (load_word(bss, i) != 0) {
        last = i;
      }
      i++;
    }

    if (!append_run(bss, start * word_bytes, (last - start + 1) * word_bytes,
                    out, capacity, &written)) {
      return beam_snapshot_survivor_overflow;
    }
  }

  /* The sub-word tail. A patched value ending there would otherwise be lost,
   * and the failure would look like a random symbol coming back zero. */
  const size_t tail_offset = nwords * word_bytes;
  for (size_t i = tail_offset; i < bss_len; i++) {
    if (bss[i] != 0) {
      if (!append_run(bss, tail_offset, bss_len - tail_offset, out, capacity,
                      &written)) {
        return beam_snapshot_survivor_overflow;
      }
      break;
    }
  }

  *used = written;
  return beam_snapshot_ok;
}

beam_survivor_read_status beam_survivor_read(const uint8_t *in, size_t len,
                                             size_t bss_len, size_t *cursor,
                                             beam_survivor_hdr_t *run,
                                             const uint8_t **payload) {
  if (*cursor == len) {
    return beam_survivor_end;
  }
  if (*cursor > len || len - *cursor < header_bytes) {
    return beam_survivor_truncated;
  }
  beam_survivor_hdr_t hdr = {};
  memcpy(&hdr, in + *cursor, header_bytes);
  if (len - *cursor - header_bytes < hdr.len) {
    return beam_survivor_truncated;
  }
  size_t end = 0;
  if (ckd_add(&end, (size_t)hdr.offset, (size_t)hdr.len) || end > bss_len) {
    return beam_survivor_out_of_bounds;
  }
  *run = hdr;
  *payload = in + *cursor + header_bytes;
  *cursor += header_bytes + hdr.len;
  return beam_survivor_record;
}

/* A usable cold-boot SP: non-zero, aligned as AAPCS64 requires at a public
 * interface, and not the top of a stack inside the writable segment the
 * reset rewrites or the snapshot region the reset runs on. */
static bool stack_top_is_valid(uintptr_t sp,
                               const beam_restart_layout *layout) {
  return sp != 0 && sp % 16 == 0 &&
         !beam_range_holds_stack_top(layout->snapshot_base,
                                     layout->snapshot_size, sp) &&
         !beam_range_holds_stack_top(layout->segment_start,
                                     layout->bss_end - layout->segment_start,
                                     sp);
}

beam_snapshot_status beam_snapshot_validate(const beam_snapshot_hdr_t *hdr,
                                            const uint8_t *survivors,
                                            size_t survivors_size,
                                            const beam_restart_layout *layout) {
  if (!beam_snapshot_captured(hdr->magic)) {
    return beam_snapshot_not_captured;
  }
  if (hdr->generation == 0 || hdr->generation == UINT64_MAX) {
    return beam_snapshot_bad_generation;
  }
  if (hdr->data_bytes != layout->bss_start - layout->segment_start) {
    return beam_snapshot_data_length_mismatch;
  }
  if (hdr->survivor_bytes > survivors_size) {
    return beam_snapshot_survivor_length_too_large;
  }
  if (!stack_top_is_valid((uintptr_t)hdr->saved_sp, layout)) {
    return beam_snapshot_bad_stack_pointer;
  }

  const size_t used = (size_t)hdr->survivor_bytes;
  const size_t bss_len = layout->bss_end - layout->bss_start;
  const size_t tail_offset = bss_len - bss_len % word_bytes;
  size_t cursor = 0;
  size_t previous_end = 0;
  for (;;) {
    beam_survivor_hdr_t run = {};
    const uint8_t *payload = nullptr;
    switch (
        beam_survivor_read(survivors, used, bss_len, &cursor, &run, &payload)) {
    case beam_survivor_end:
      return beam_snapshot_ok;
    case beam_survivor_truncated:
      return beam_snapshot_survivor_truncated;
    case beam_survivor_out_of_bounds:
      return beam_snapshot_survivor_out_of_bounds;
    case beam_survivor_record:
      break;
    }
    if (run.len == 0) {
      return beam_snapshot_survivor_empty;
    }
    /* Word runs start and end on word boundaries. Only the sub-word tail may
     * end elsewhere, and it always ends exactly at the end of .bss. */
    const bool is_tail =
        run.offset == tail_offset && (size_t)run.offset + run.len == bss_len;
    if (run.offset % word_bytes != 0 ||
        (run.len % word_bytes != 0 && !is_tail)) {
      return beam_snapshot_survivor_misaligned;
    }
    if (run.offset < previous_end) {
      return beam_snapshot_survivor_out_of_order;
    }
    previous_end = (size_t)run.offset + run.len;
  }
}

const char *beam_snapshot_status_name(beam_snapshot_status status) {
  switch (status) {
  case beam_snapshot_ok:
    return "ok";
  case beam_snapshot_survivor_overflow:
    return "survivor-area-overflow";
  case beam_snapshot_bss_too_large:
    return "bss-too-large";
  case beam_snapshot_not_captured:
    return "reset-without-snapshot";
  case beam_snapshot_bad_generation:
    return "bad-generation";
  case beam_snapshot_data_length_mismatch:
    return "data-length-mismatch";
  case beam_snapshot_survivor_length_too_large:
    return "survivor-length-too-large";
  case beam_snapshot_bad_stack_pointer:
    return "bad-stack-pointer";
  case beam_snapshot_survivor_truncated:
    return "survivor-truncated";
  case beam_snapshot_survivor_out_of_bounds:
    return "survivor-out-of-bounds";
  case beam_snapshot_survivor_empty:
    return "survivor-empty";
  case beam_snapshot_survivor_misaligned:
    return "survivor-misaligned";
  case beam_snapshot_survivor_out_of_order:
    return "survivor-out-of-order";
  }
  return "unknown";
}
