#ifndef CHRYSOPOLIS_ROOT_COMMAND_H
#define CHRYSOPOLIS_ROOT_COMMAND_H 1

/* Root's PPC command decisions: frame checks, the token bucket, retry
 * fencing and reply values. Pure: no globals, allocation, logging or Microkit
 * calls, so Root links it and tests/host runs it. The adapter in main.c reads
 * the message registers, performs the decided action and replies. Not
 * thread-safe: Root's callbacks are serialized.
 *
 * Retry contract, one row per opcode (CtlCommand in orchestrator_abi.zig
 * pins the words):
 *
 *   opcode         class           duplicate     stale    lost reply
 *   HELLO          read-only       repeatable    n/a      retry freely
 *   SYNC           read-only       repeatable    n/a      retry freely
 *   RESTART_CHILD  preconditioned  already_done  stale    already_done
 *   slot opcodes   refused until pool slots exist (B6)
 *
 * The fence is the published status seq: a completed command publishes, so
 * the seq it observed never matches again. Only the last completed command
 * is remembered, which suffices because a PPC caller blocks until Root
 * replies and so has at most one command in flight. */
#include "root_policy.h"
#include <chrysopolis/root_control.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static constexpr size_t root_command_words = 4;

typedef enum : uint8_t {
  root_command_none,
  /* Restart decision.child through the shared budget path. */
  root_command_restart,
} root_command_action;

/* The bucket's refill machine. running accrues tokens from clock samples;
 * an unavailable or backwards sample moves it to off, which is absorbing:
 * only tokens already held are spent for the rest of the incarnation. */
typedef enum : uint8_t {
  root_command_refill_running,
  root_command_refill_off,
} root_command_refill;

/* The retry fence. empty until an action completes, then completed with
 * that command's words and reply; each completion replaces the last. */
typedef enum : uint8_t {
  root_command_fence_empty,
  root_command_fence_completed,
} root_command_fence;

typedef struct {
  root_command_refill refill;
  uint32_t capacity;
  uint32_t tokens;
  uint64_t interval_ticks;
  uint64_t last_ticks;
  root_command_fence fence;
  uint16_t memo_opcode;
  uint64_t memo_args[root_command_words];
  uint64_t memo_values[root_command_words];
  /* Reply tallies SYNC reports. accepted: ok and already_done. */
  uint64_t accepted;
  uint64_t rate_limited;
  uint64_t rejected;
} root_command_state;

/* Root's state as the command sees it, sampled by the adapter per call. */
typedef struct {
  uint32_t abi_version;
  uint64_t root_generation;
  uint64_t beam_incarnation;
  uint64_t cntfrq;
  /* The last even seq on the page, meaningful only while publishing. */
  uint64_t seq;
  bool publishing;
  /* Children present in this image, and those RESTART_CHILD may address. */
  uint64_t present_mask;
  uint64_t driver_mask;
  /* child_count records indexed by child id. */
  size_t child_count;
  const root_child_record *records;
} root_command_view;

typedef struct {
  chryso_ctl_result result;
  root_command_action action;
  root_child child;
  uint16_t opcode;
  uint64_t args[root_command_words];
  /* Reply values; for root_command_restart the adapter fills them. */
  uint64_t values[root_command_words];
} root_command_decision;

/* Starts with a full bucket of capacity tokens refilled evenly over
 * refill_ticks, and an empty fence. refill_ticks 0, capacity 0 or !now_valid
 * starts the refill machine off; capacity 0 rate-limits everything. state
 * must be non-null. */
void root_command_init(root_command_state *state, uint32_t capacity,
                       uint64_t refill_ticks, uint64_t now, bool now_valid);

/* Checks the transport before any message register is read: the PPC
 * channel, a label that is exactly a declared nonzero opcode, and exactly
 * root_command_words words. Returns ok to proceed, otherwise the reply
 * result (refused for a foreign channel, bad_arg otherwise), counted as
 * rejected. No token is spent. */
[[__nodiscard__]] chryso_ctl_result
root_command_frame(root_command_state *state, unsigned int channel,
                   unsigned int pp_channel, uint64_t label, uint64_t count);

/* Decides a framed command. Unused words must be zero (bad_arg, no token);
 * then one token is spent or rate_limited returned; then the opcode's
 * preconditions run. A rejection never carries an action, so it reaches no
 * capability. For root_command_restart the adapter acts, fills values and
 * calls root_command_complete. opcode must have passed root_command_frame;
 * state, view, view->records and args must be non-null. */
[[__nodiscard__]] root_command_decision
root_command_decide(root_command_state *state, const root_command_view *view,
                    uint16_t opcode,
                    const uint64_t args[static root_command_words],
                    uint64_t now, bool now_valid);

/* Records a performed action's reply so an identical retry is answered
 * already_done. decision must come from root_command_decide with an action;
 * otherwise nothing changes. */
void root_command_complete(root_command_state *state,
                           const root_command_decision *decision);

#endif
