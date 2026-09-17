/*
 * .bss is tens of megabytes and all but a few dozen bytes of it are zero, so
 * the scan loads a word at a time. The load uses __builtin_memcpy because the
 * runtime builds with -ffreestanding, which stops the compiler from treating a
 * plain memcpy call as the inlinable builtin; a real call per word would cost
 * most of a second under TCG. Loading through memcpy rather than a cast also
 * keeps the scan valid for a buffer of any alignment.
 */
#include "beam_snapshot_codec.h"

#include <string.h>

static constexpr size_t word_bytes = sizeof(uint64_t);
static constexpr size_t header_bytes = sizeof(beam_survivor_hdr_t);

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

bool beam_survivor_next(const uint8_t *in, size_t len, size_t *cursor,
                        beam_survivor_hdr_t *run, const uint8_t **payload) {
  if (*cursor > len || len - *cursor < header_bytes) {
    return false;
  }
  memcpy(run, in + *cursor, header_bytes);
  *payload = in + *cursor + header_bytes;
  *cursor += header_bytes + run->len;
  return true;
}
