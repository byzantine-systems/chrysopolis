#ifndef CHRYSOPOLIS_BEAM_SNAPSHOT_CODEC_H
#define CHRYSOPOLIS_BEAM_SNAPSHOT_CODEC_H 1

#include "beam_restart_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Encoding and validation of beam_server's warm-restart snapshot. Pure and
 * pre-libc safe: no globals, no logging, no allocation. Callers own every
 * buffer and decide what a failure means (restart.c faults to Root).
 */

/* Written last on the cold-boot path, so a torn capture reads as "cold". The
 * bytes spell "CHRYSNP1". */
static constexpr uint64_t beam_snapshot_magic = 0x43485259534E5031u;

/* The header at offset 0 of the snapshot region. The layout is part of the
 * restart ABI. */
typedef struct {
  uint64_t magic;
  uint64_t generation;     /* 1 on cold boot, incremented by every reset */
  uint64_t saved_sp;       /* the cold-boot SP, so a reset can restore it */
  uint64_t data_bytes;     /* used length of the data area */
  uint64_t survivor_bytes; /* used length of the survivor area */
} beam_snapshot_hdr_t;

static_assert(sizeof(beam_snapshot_hdr_t) == 5 * sizeof(uint64_t),
              "snapshot header must remain five packed 64-bit words");
static_assert(_Alignof(beam_snapshot_hdr_t) == _Alignof(uint64_t),
              "snapshot header must remain 64-bit aligned");
static_assert(offsetof(beam_snapshot_hdr_t, magic) == 0 &&
                  offsetof(beam_snapshot_hdr_t, generation) == 8 &&
                  offsetof(beam_snapshot_hdr_t, saved_sp) == 16 &&
                  offsetof(beam_snapshot_hdr_t, data_bytes) == 24 &&
                  offsetof(beam_snapshot_hdr_t, survivor_bytes) == 32,
              "snapshot header field offsets are part of the restart ABI");

/* One discovered non-zero run of .bss, followed in the survivor area by len
 * bytes of payload. The layout is part of the restart ABI. */
typedef struct {
  uint32_t offset; /* from the start of .bss */
  uint32_t len;
} beam_survivor_hdr_t;

static_assert(sizeof(beam_survivor_hdr_t) == 8,
              "survivor record header must remain eight bytes");
static_assert(_Alignof(beam_survivor_hdr_t) == _Alignof(uint32_t),
              "survivor record header must remain 32-bit aligned");
static_assert(offsetof(beam_survivor_hdr_t, offset) == 0 &&
                  offsetof(beam_survivor_hdr_t, len) == 4,
              "survivor record offsets are part of the restart ABI");

typedef enum {
  beam_snapshot_ok,
  /* The records do not fit the survivor area. */
  beam_snapshot_survivor_overflow,
  /* The .bss is too large for 32-bit record offsets. */
  beam_snapshot_bss_too_large,
  /* No completed capture: the magic is missing. */
  beam_snapshot_not_captured,
  /* Generation 0 was never written by a capture, and UINT64_MAX cannot be
   * incremented. */
  beam_snapshot_bad_generation,
  /* The data length differs from the running image's file-backed data. */
  beam_snapshot_data_length_mismatch,
  /* The used survivor length exceeds the survivor area. */
  beam_snapshot_survivor_length_too_large,
  /* The saved SP is zero, misaligned, or inside memory the reset rewrites. */
  beam_snapshot_bad_stack_pointer,
  /* A record header or payload runs past the used survivor length. */
  beam_snapshot_survivor_truncated,
  /* A record ends past the end of .bss. */
  beam_snapshot_survivor_out_of_bounds,
  /* A record has no payload. */
  beam_snapshot_survivor_empty,
  /* A record is not word-aligned and is not the sub-word tail. */
  beam_snapshot_survivor_misaligned,
  /* A record starts before the previous one ends. */
  beam_snapshot_survivor_out_of_order,
} beam_snapshot_status;

/*
 * Record every non-zero run of bss[0, bss_len) into out[0, capacity).
 *
 * The scan is word-wise: a run spans whole 8-byte words, and zero words at
 * most gap_words apart stay inside one run so a struct with zero fields is one
 * record. Bytes past the last whole word form their own record when any of
 * them is non-zero. On ok, *used receives the encoded length. On failure the
 * contents of out are unspecified and *used is not written. bss may be null
 * only when bss_len is 0; out and used must be non-null.
 */
[[__nodiscard__]] beam_snapshot_status
beam_survivors_encode(const uint8_t *bss, size_t bss_len, size_t gap_words,
                      uint8_t *out, size_t capacity, size_t *used);

typedef enum {
  /* *run and *payload describe the next record; *cursor moved past it. */
  beam_survivor_record,
  /* *cursor is exactly at len: no records remain. */
  beam_survivor_end,
  /* The next header or its payload does not fit in len. */
  beam_survivor_truncated,
  /* The next record ends past bss_len. */
  beam_survivor_out_of_bounds,
} beam_survivor_read_status;

/*
 * Read the record at *cursor from an encoded survivor area of used length
 * len, for a .bss of bss_len bytes. Only beam_survivor_record writes *run,
 * *payload and *cursor; every other result leaves all three untouched. All
 * pointers must be non-null.
 */
[[__nodiscard__]] beam_survivor_read_status
beam_survivor_read(const uint8_t *in, size_t len, size_t bss_len,
                   size_t *cursor, beam_survivor_hdr_t *run,
                   const uint8_t **payload);

/*
 * Validate a captured snapshot against the running image before a restore
 * writes anything. layout must already have passed beam_restart_layout_check;
 * the data length, .bss length and writable segment come from it. survivors
 * points at the survivor area of survivors_size bytes. Checks run in the
 * order of the status enumerators, and the survivor walk accepts exactly
 * what beam_survivors_encode produces for that .bss length.
 */
[[__nodiscard__]] beam_snapshot_status
beam_snapshot_validate(const beam_snapshot_hdr_t *hdr, const uint8_t *survivors,
                       size_t survivors_size,
                       const beam_restart_layout *layout);

/* The reason logged for a snapshot status. */
[[__nodiscard__]] const char *
beam_snapshot_status_name(beam_snapshot_status status);

/* True when the header's magic says a cold-boot capture completed. */
[[__nodiscard__]] static inline bool beam_snapshot_captured(uint64_t magic) {
  return magic == beam_snapshot_magic;
}

/* True once the PD has been reset at least once: a completed capture whose
 * generation has advanced past the cold boot's 1. */
[[__nodiscard__]] static inline bool beam_snapshot_warm(uint64_t magic,
                                                        uint64_t generation) {
  return beam_snapshot_captured(magic) && generation > 1;
}

/* The unmapped address whose store carries status's low byte to Root. */
[[__nodiscard__]] static inline uintptr_t beam_exit_fault_addr(uintptr_t base,
                                                               int status) {
  return base + (unsigned int)(status & 0xff);
}

#endif
