#ifndef CHRYSOPOLIS_BEAM_SNAPSHOT_CODEC_H
#define CHRYSOPOLIS_BEAM_SNAPSHOT_CODEC_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Encoding of beam_server's warm-restart snapshot. Pure and pre-libc safe: no
 * globals, no logging, no allocation. Callers own every buffer and decide
 * what a failure means (restart.c parks the PD).
 */

/* Written last on the cold-boot path, so a torn capture reads as "cold". The
 * bytes spell "CHRYSNP1". */
static constexpr uint64_t beam_snapshot_magic = 0x43485259534E5031u;

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

typedef enum {
  beam_snapshot_ok,
  /* The records do not fit the survivor area. */
  beam_snapshot_survivor_overflow,
  /* The .bss is too large for 32-bit record offsets. */
  beam_snapshot_bss_too_large,
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

/*
 * Iterate the records of an encoded survivor area of length len. Starting
 * from *cursor 0, each call reads the record header at *cursor into *run,
 * points *payload at its len bytes, advances *cursor past them and returns
 * true. It returns false, writing nothing, once fewer than a header's bytes
 * remain. Record lengths are not checked against len or any .bss size.
 */
[[__nodiscard__]] bool beam_survivor_next(const uint8_t *in, size_t len,
                                          size_t *cursor,
                                          beam_survivor_hdr_t *run,
                                          const uint8_t **payload);

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
