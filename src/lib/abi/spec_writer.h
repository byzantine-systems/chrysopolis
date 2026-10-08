#ifndef CHRYSOPOLIS_SPEC_WRITER_H
#define CHRYSOPOLIS_SPEC_WRITER_H 1

/* Writer side of the orchestrator_spec page, one store group per call so a
 * test or fault injector can stop the commit after any step. The steps are
 * the protocol on SpecPage in interfaces/orchestrator_abi.zig and the
 * spec_bank model in tools/abi/publication.zig: open, write, seal, close,
 * flip. Pure: no globals, allocation or logging. Not thread-safe: one writer
 * owns the page. The reader side is src/pd/root/spec.h. */
#include <chrysopolis/root_control.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum : uint8_t {
  spec_writer_phase_idle,
  /* The target bank's sequence is odd. */
  spec_writer_phase_open,
  spec_writer_phase_written,
  spec_writer_phase_sealed,
  /* The target bank's sequence is the next even value. */
  spec_writer_phase_closed,
  /* active_bank selects the target bank; begin may start the next commit. */
  spec_writer_phase_flipped,
} spec_writer_phase;

typedef enum : uint8_t {
  spec_write_ok,
  /* The call does not follow the current phase. */
  spec_write_bad_state,
  /* Null or misaligned page, or a header that is neither zero nor valid. */
  spec_write_bad_page,
  /* The target bank cannot take another odd/even pair without wrapping. */
  spec_write_exhausted,
} spec_write_result;

typedef struct {
  chryso_spec_page *page;
  spec_writer_phase phase;
  uint32_t bank;
  /* The odd value open stores; close stores open + 1. */
  uint32_t open_seq;
  uint64_t generation;
  uint32_t budget[chryso_child_count];
  uint8_t desired[chryso_child_count];
} spec_writer;

/* An idle writer. writer must be non-null. */
void spec_writer_init(spec_writer *writer);

/*
 * Prepares a commit of generation into the inactive bank of page, copying
 * budget[] and desired[]. A zero header is initialized first (magic, version,
 * active_bank 0); that is the only store begin makes. Allowed from idle or
 * flipped. Refusals return before any store and leave the writer's phase.
 * The generation is not checked against the page: ordering generations is
 * the caller's policy.
 */
[[__nodiscard__]] spec_write_result
spec_writer_begin(spec_writer *writer, chryso_spec_page *page,
                  uint64_t generation,
                  const uint32_t budget[static chryso_child_count],
                  const uint8_t desired[static chryso_child_count]);

/* The protocol steps, in commit order. */
typedef enum : uint8_t {
  spec_writer_step_open,
  spec_writer_step_write,
  spec_writer_step_seal,
  spec_writer_step_close,
  spec_writer_step_flip,
} spec_writer_step_kind;

static constexpr size_t spec_writer_step_count = 5;

/* Runs one step. A step is allowed only from the phase the transition table
 * in spec_writer.c names for it; otherwise, or for an undeclared step, it
 * returns spec_write_bad_state and stores nothing. */
[[__nodiscard__]] spec_write_result
spec_writer_step(spec_writer *writer, spec_writer_step_kind step);

/* begin then every step, stopping at the first refusal. */
[[__nodiscard__]] spec_write_result
spec_writer_commit(spec_writer *writer, chryso_spec_page *page,
                   uint64_t generation,
                   const uint32_t budget[static chryso_child_count],
                   const uint8_t desired[static chryso_child_count]);

#endif
