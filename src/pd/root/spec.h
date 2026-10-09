#ifndef CHRYSOPOLIS_ROOT_SPEC_H
#define CHRYSOPOLIS_ROOT_SPEC_H 1

/* Reader side of the BEAM's orchestrator_spec page. Pure: no globals,
 * allocation, logging or Microkit calls, so Root links it and tests/host runs
 * it. The writer side is src/lib/abi/spec_writer.h. Not thread-safe: Root's
 * callbacks are serialized. */
#include <chrysopolis/root_control.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The last policy Root applied. Generation 0 is the compiled-in policy and
 * leaves every other field zero. budget[] and desired[] are the raw bank
 * bytes, zero for absent children. They are clamped only at use
 * (root_spec_budget, root_spec_desired), so an equal generation compares
 * byte for byte, and an undeclared desired value never becomes an enum. */
typedef struct {
  uint64_t generation;
  uint32_t bank;
  uint32_t crc32;
  uint32_t budget[chryso_child_count];
  uint8_t desired[chryso_child_count];
} root_spec_policy;

typedef enum : uint8_t {
  /* No page, a zero header, or no bank ever published a nonzero
   * generation: the retained policy stays. */
  root_spec_absent,
  /* A newer bank replaced the retained policy. */
  root_spec_applied,
  /* A bank matched the retained generation and content. */
  root_spec_unchanged,
  /* Nothing eligible: the retained policy stays. */
  root_spec_kept,
} root_spec_outcome;

typedef struct {
  root_spec_outcome outcome;
  /* A rejection the caller has not been told about: publish and log it. */
  bool record;
  /* Why the header or active bank was not used, or unset. */
  chryso_root_spec_reject reason;
  uint64_t generation;
  uint32_t bank;
} root_spec_result;

typedef struct {
  root_spec_policy retained;
  /* Last recorded rejection, so a page that stays bad is reported once. */
  chryso_root_spec_reject last_reason;
  uint64_t last_generation;
  uint32_t last_bank;
} root_spec_state;

/* Resets state to the compiled-in policy. state must be non-null. */
void root_spec_init(root_spec_state *state);

/*
 * Reads the shared page once and decides which policy Root uses.
 *
 * The page is copied into scratch between acquire reads of active_bank and
 * both bank_seq words, and only scratch is validated and decoded. The active
 * bank is preferred, then the other bank; a bank is eligible when it passes
 * the generated checker, declares nothing for a child outside present_mask,
 * and is newer than the retained generation or equal with identical
 * budget[] and desired[]. retained changes only on root_spec_applied, in one
 * assignment. A rejection of the header or active bank is reported once per
 * (reason, generation, bank); an inactive bank mid-write is never reported.
 *
 * shared == nullptr means no page is mapped and yields root_spec_absent.
 * Bits of present_mask at or above chryso_child_count are ignored.
 * Checked: state and scratch must be non-null (otherwise root_spec_absent
 * with nothing written). Unchecked: scratch must not overlap shared, and
 * shared must point at a whole mapped page.
 * Cost: one 4 KiB copy and at most two bank checks; no allocation.
 */
[[__nodiscard__]] root_spec_result
root_spec_consume(root_spec_state *state, const chryso_spec_page *shared,
                  chryso_spec_page *scratch, uint64_t present_mask);

/* The lifetime ceiling for child: compiled when no spec applies or its entry
 * is zero, otherwise the spec value capped at hard_max. */
[[__nodiscard__]] uint32_t root_spec_budget(const root_spec_state *state,
                                            unsigned int child,
                                            uint32_t compiled,
                                            uint32_t hard_max);

/* The desired state Root reports for child. Only children in slot_mask may
 * be anything but running; until pool slots exist that mask is zero. */
[[__nodiscard__]] chryso_root_desired
root_spec_desired(const root_spec_state *state, unsigned int child,
                  uint64_t slot_mask);

/* A stable lower-case name for logs; "invalid" for an undeclared value. */
[[__nodiscard__]] const char *
root_spec_reject_name(chryso_root_spec_reject reason);

#endif
