/*
 * main.c - Chrysopolis Root fault-handler / process-manager PD.
 *
 * The Root PD is the seL4-level substrate for the "crash and restart"
 * resilience model (Crashing for Reliability, seL4 Summit 2023). It is the
 * PARENT of the restartable child PDs in the generated SDF (see
 * tools/sdf/system.zig: root.addChild(&driver, .{})). Microkit routes a
 * child's fault to the parent's fault() callback, where we log the fault,
 * apply a per-child restart budget, and either restart the child to a clean
 * entry or give up and stop it.
 *
 * Deliberately dependency-free: only <microkit.h>, no sDDF util, no libc. The
 * error kernel stays small: it holds restart policy + mechanism only, never
 * application state.
 *
 * Restart mechanics:
 *   - microkit_pd_restart(child, entry) only rewrites the child's PC and
 *     resumes it. It does NOT re-zero .bss or reload .data; that happens once,
 *     at boot, in the Microkit loader. Re-entering the entry point re-runs
 *     _start, main, then init() with the child's memory as the crash left it,
 *     so each driver's init() must be idempotent (that is the driver-restart
 *     work, not this PD).
 *   - `entry` is the child ELF's e_entry (== _start). It is a property of the
 *     board's microkit.ld (ENTRY(_start), image based at 0x200000 on every
 *     current board), NOT a universal constant, so it is never hardcoded: it
 *     arrives in the .restart_config section below, patched per-board at image
 *     assembly time. See that section's comment for the mechanism.
 *   - beam_server is the exception, and it is resumed at its own _reset symbol
 *     instead. "init() must be idempotent" is achievable for a driver holding a
 *     few .bss words; beam_server holds ~28 MiB of ERTS, libc and cothread
 *     state, so it resets that memory itself before re-entering the normal boot
 *     path. Root's part is only knowing which of the two entry points to use;
 *     the mechanism lives in src/pd/beam/restart/restart.c.
 *
 * Identifiers:
 *   - Microkit child ids and channel ids are separate id spaces
 *     (BASE_TCB_CAP versus BASE_OUTPUT_NOTIFICATION_CAP) that are both plain
 *     unsigned ints. Past the entry points Root carries them as root_child and
 *     root_channel (root_policy.h), and a root_child exists only once the raw
 *     id has been checked against the set of children system_abi.zig
 *     declares. An id outside that set never reaches a capability invocation.
 *
 * Child lifecycle:
 *   - Every child starts live with zero restart counts. Production still uses
 *     the original lifetime budget until a validated orchestration policy can
 *     select the timed path. A separate test Root exercises that path now.
 *   - When the selected budget is spent (or there is no entry to restart at)
 * Root stops the child, marks it gone and tells its dependents once. Gone is
 *     terminal: later faults, debug restarts and fault injections for that
 *     child are logged as ignored and make no Microkit call.
 */
#include "root_policy.h"

#include <runtime_abi.h>

#include <microkit.h>

#include <stddef.h>
#include <stdint.h>

/* Compile-time fallback child ELF entry point from system_abi.zig. Image
 * assembly replaces it with the actual linked child entry after checking that
 * every restartable child agrees. The fallback covers a by-hand `zig build`
 * plus `microkit` run where nothing patches the section. */
/* Entry points to resume children at, patched per-board at image assembly.
 * `volatile` + `used` + its own section keep the compiler from folding them and
 * let objcopy overwrite the pair, the same mechanism sDDF uses for its per-PD
 * config blobs.
 *
 * [0] restart_entry: the shared child ELF entry point (== _start). It is a
 *     property of the board's microkit.ld (ENTRY(_start)), NOT a universal
 *     constant, so modules/images.nix reads e_entry from the linked child ELFs,
 *     asserts it is uniform across them, and objcopies the value in.
 *
 * [1] beam_reset_entry: beam_server's _reset symbol, which is NOT its ELF entry
 *     point. Unlike a driver, beam_server cannot be resumed at _start: nothing
 *     in a Microkit restart re-zeroes .bss or reloads .data, and beam_server
 *     carries ~28 MiB of ERTS and libc state that has to be pristine before the
 *     emulator can boot again. _reset is the trampoline that restores that
 *     memory and then enters the normal boot (see
 * src/pd/beam/restart/restart.c). modules/images.nix resolves that symbol with
 * llvm-nm and patches it in.
 *
 * The initializers are the fallback for an un-patched build. A zero
 * beam_reset_entry means "not patched", which root treats as "beam_server is
 * not restartable in this image" rather than jumping to address 0. */
typedef struct {
  uint64_t restart_entry;
  uint64_t beam_reset_entry;
} root_restart_config_t;

_Static_assert(sizeof(root_restart_config_t) ==
                   ROOT_RESTART_CONFIG_WORDS * ROOT_RESTART_CONFIG_WORD_BYTES,
               "Root restart config size must match the image patch payload");
_Static_assert(_Alignof(root_restart_config_t) == _Alignof(uint64_t),
               "Root restart config must remain 64-bit aligned");
_Static_assert(offsetof(root_restart_config_t, restart_entry) == 0,
               "child restart entry must be the first patch word");
_Static_assert(offsetof(root_restart_config_t, beam_reset_entry) ==
                   ROOT_RESTART_CONFIG_WORD_BYTES,
               "BEAM reset entry must be the second patch word");
_Static_assert(sizeof(seL4_Word) == ROOT_RESTART_CONFIG_WORD_BYTES,
               "restart entries must have the target word width");

__attribute__((__section__(ROOT_RESTART_CONFIG_SECTION),
               used)) volatile root_restart_config_t restart_config = {
    .restart_entry = MICROKIT_RESTART_ENTRY,
    .beam_reset_entry = 0,
};

#define restart_entry (restart_config.restart_entry)
#define beam_reset_entry (restart_config.beam_reset_entry)

/*
 * The children system_abi.zig declares, one bit per child id. fault() only
 * turns a raw id into a root_child when its bit is set, so the budget table is
 * never indexed and BASE_TCB_CAP + id is never invoked for anything else.
 * The crasher's bit is set in every image; in production no PD holds that
 * badge, so no fault can arrive with it.
 */
static constexpr uint64_t root_known_children =
    (UINT64_C(1) << ROOT_CHILD_SERIAL) | (UINT64_C(1) << ROOT_CHILD_TIMER) |
    (UINT64_C(1) << ROOT_CHILD_BLK) | (UINT64_C(1) << ROOT_CHILD_ETH) |
    (UINT64_C(1) << ROOT_CHILD_CRASHER) | (UINT64_C(1) << ROOT_CHILD_BEAM);

static_assert(ROOT_MAX_CHILDREN <= root_child_mask_bits,
              "every child id must have a bit in the known-child mask");
static_assert(ROOT_CHILD_SERIAL < ROOT_MAX_CHILDREN &&
                  ROOT_CHILD_TIMER < ROOT_MAX_CHILDREN &&
                  ROOT_CHILD_BLK < ROOT_MAX_CHILDREN &&
                  ROOT_CHILD_ETH < ROOT_MAX_CHILDREN &&
                  ROOT_CHILD_CRASHER < ROOT_MAX_CHILDREN &&
                  ROOT_CHILD_BEAM < ROOT_MAX_CHILDREN,
              "every child id must fit the restart budget table");
/* A sum of distinct powers of two equals their OR; a repeated id carries. */
static_assert((UINT64_C(1) << ROOT_CHILD_SERIAL) +
                      (UINT64_C(1) << ROOT_CHILD_TIMER) +
                      (UINT64_C(1) << ROOT_CHILD_BLK) +
                      (UINT64_C(1) << ROOT_CHILD_ETH) +
                      (UINT64_C(1) << ROOT_CHILD_CRASHER) +
                      (UINT64_C(1) << ROOT_CHILD_BEAM) ==
                  root_known_children,
              "child ids must be distinct");
/* microkit_pd_restart and microkit_pd_stop invoke BASE_TCB_CAP + id. Past the
 * TCB block the slots hold VM TCB caps, which must be unreachable. */
static_assert(BASE_TCB_CAP + ROOT_MAX_CHILDREN <= BASE_VM_TCB_CAP,
              "every child id must address a TCB cap slot");

/*
 * Test-only debug-restart channels (present only in the restart image, which
 * tools/sdf/system.zig generates with --with-restart-debug; production SDFs
 * wire none of these channels, so these ids are simply never signalled).
 *
 * Two channels per restartable driver class, because a Microkit notification
 * carries no payload: the channel the signal arrives on IS the request. The
 * first group lets a test restart a HEALTHY driver on demand. The second group
 * resumes that driver at address 0, making the real driver PD take an
 * instruction fault that returns through fault() and the ordinary restart
 * policy. Both channel and child ids come from system_abi.zig and map 1:1.
 *
 * Note these are microkit_channel ids, a SEPARATE id space from the
 * microkit_child ids they map to (BASE_OUTPUT_NOTIFICATION_CAP vs
 * BASE_TCB_CAP).
 */
static constexpr root_channel_layout root_channels = {
    .debug_first = ROOT_DEBUG_CH_SERIAL,
    .fault_first = ROOT_FAULT_CH_SERIAL,
    .count = ROOT_DEBUG_CH_MAX - ROOT_DEBUG_CH_SERIAL + 1,
};

/* The rules root_channel_layout_valid checks, held against the constants. */
static_assert(ROOT_DEBUG_CH_TIMER == ROOT_DEBUG_CH_SERIAL + 1 &&
                  ROOT_DEBUG_CH_BLK == ROOT_DEBUG_CH_SERIAL + 2 &&
                  ROOT_DEBUG_CH_ETH == ROOT_DEBUG_CH_SERIAL + 3 &&
                  ROOT_DEBUG_CH_MAX == ROOT_DEBUG_CH_ETH,
              "debug-restart channels must be dense in driver-class order");
static_assert(ROOT_FAULT_CH_TIMER == ROOT_FAULT_CH_SERIAL + 1 &&
                  ROOT_FAULT_CH_BLK == ROOT_FAULT_CH_SERIAL + 2 &&
                  ROOT_FAULT_CH_ETH == ROOT_FAULT_CH_SERIAL + 3 &&
                  ROOT_FAULT_CH_MAX == ROOT_FAULT_CH_ETH,
              "fault-injection channels must be dense in driver-class order");
static_assert(ROOT_DEBUG_CH_MAX < MICROKIT_MAX_CHANNELS &&
                  ROOT_FAULT_CH_MAX < MICROKIT_MAX_CHANNELS,
              "restart channels must be valid Microkit channel ids");
static_assert(ROOT_DEBUG_CH_MAX < ROOT_FAULT_CH_SERIAL ||
                  ROOT_FAULT_CH_MAX < ROOT_DEBUG_CH_SERIAL,
              "debug-restart and fault-injection channels must not overlap");
static_assert((ROOT_GONE_CH_BLK < ROOT_DEBUG_CH_SERIAL ||
               ROOT_GONE_CH_BLK > ROOT_DEBUG_CH_MAX) &&
                  (ROOT_GONE_CH_BLK < ROOT_FAULT_CH_SERIAL ||
                   ROOT_GONE_CH_BLK > ROOT_FAULT_CH_MAX),
              "the give-up channel must not be a restart channel");
static_assert(ROOT_GONE_CH_BLK < MICROKIT_MAX_CHANNELS &&
                  ROOT_GONE_CH_NONE >= MICROKIT_MAX_CHANNELS,
              "the give-up channel must be valid and its sentinel must not");

/* Driver-class index -> child id, for both channel groups. Built from the ABI
 * constants checked above rather than through root_child_from_raw. */
static const root_child
    root_class_child[ROOT_DEBUG_CH_MAX - ROOT_DEBUG_CH_SERIAL + 1] = {
        {.value = ROOT_CHILD_SERIAL},
        {.value = ROOT_CHILD_TIMER},
        {.value = ROOT_CHILD_BLK},
        {.value = ROOT_CHILD_ETH},
};

/*
 * Give-up notification channels, shared through system_abi.zig.
 *
 * Unlike the debug-restart channels above these exist in EVERY image, including
 * production. They carry the one thing only root can report: that a child has
 * been stopped for good and is never coming back.
 *
 * That report cannot come from the driver, which is precisely what has stopped
 * running, and it cannot be inferred by the dependent either. A blk virtualiser
 * waiting on the driver's init generation to change sees exactly the same thing
 * whether the driver is slow to come back or has been stopped permanently, so
 * without this signal it waits forever and every client blocked on an
 * outstanding request waits with it. sDDF's own design document lists the same
 * hole as an open limitation of its hotplug design: "Initialisation failures
 * are not communicated to the clients" (sDDF Design, Release 0.6, S6.4).
 *
 * Only blk is wired today. It is the class where the silence is fatal rather
 * than degrading: a lost completion parks the fatfs worker that issued it, and
 * beam_server's fs_blocking_wait() polls without yielding, so one unanswered
 * request takes down the whole image. The other classes degrade instead (a dead
 * console is silent, a dead NIC drops traffic), so they are left unwired rather
 * than given a channel with no listener.
 */
/*
 * Which channel to signal when a given child is stopped for good, or
 * ROOT_GONE_CH_NONE for a child whose dependents have nothing to recover.
 *
 * A switch rather than a table because the "no channel" case must be the
 * default: a designated initialiser array would leave every unlisted child at
 * 0, which is a perfectly valid channel id, and signalling an unwired one is a
 * cap fault in the PD that is supposed to be handling faults.
 *
 * beam_server is deliberately absent. Nothing in the system depends on it the
 * way fatfs depends on blk, so there is no dependent to inform; when root gives
 * up on beam_server the giveup log line is the system's obituary, because the
 * component that would have reported anything is the one that just stopped.
 */
static root_channel root_gone_channel(root_child child) {
  switch (child.value) {
  case ROOT_CHILD_BLK:
    return (root_channel){.value = ROOT_GONE_CH_BLK};
  default:
    return (root_channel){.value = ROOT_GONE_CH_NONE};
  }
}

/* Restart budget for a driver PD. After this many restarts we stop the child
 * rather than spin forever (the reliability talk's "giving up" decision). A
 * time-windowed budget (reset the count after the child stays up for a while)
 * is a future refinement: it needs a timer channel wired into the Root PD. */
/* beam_server's budget is larger because its restarts are not all failures. A
 * driver restart always means a driver went wrong, but a BEAM PD restart is
 * also what an ordinary `init:stop()` at the shell produces, and eight of those
 * should not permanently stop the system. The budget still exists: an ERTS that
 * faults on every boot is exactly the runaway this bounds, and so is a
 * snapshot that fails validation at _reset. */
/*
 * A switch rather than a table, for the same reason root_gone_channel above is
 * one: with a designated-initialiser array every unlisted child would default
 * to a budget of 0, which does not read as "unlisted", it reads as "give up on
 * the first fault". The default has to be the driver budget, and only a switch
 * makes that the default.
 */
static unsigned int root_restart_budget(root_child child) {
  switch (child.value) {
  case ROOT_CHILD_BEAM:
    return ROOT_BEAM_RESTART_BUDGET;
  default:
    return ROOT_RESTART_BUDGET;
  }
}

/*
 * Where to resume a given child. Drivers re-enter at the shared ELF entry point
 * and re-run their idempotent init(); beam_server re-enters at its _reset
 * trampoline, which restores its memory image first. Returns 0 when the child
 * has no usable entry, which the decision treats as "cannot restart this one".
 */
static seL4_Word root_restart_entry(root_child child) {
  if (child.value == ROOT_CHILD_BEAM) {
    return (seL4_Word)beam_reset_entry;
  }
  return (seL4_Word)restart_entry;
}

/* Indexed by root_child.value, which root_child_from_raw bounds by
 * ROOT_MAX_CHILDREN. */
static root_child_record child_records[ROOT_MAX_CHILDREN];

/* The generic timer's physical counter is already read by the sDDF timer PD
 * and beam_server on this board. Root reads it directly at EL0: a PPC to the
 * timer driver would violate Microkit's priority rule and make fault policy
 * depend on a child Root may need to restart. The pure checks and arithmetic
 * stay in root_policy.h. No timer notification is wired to Root. */
static root_clock root_clock_state;

static inline uint64_t root_now_ticks(void) {
  uint64_t ticks;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
  return ticks;
}

static inline uint64_t root_clock_freq(void) {
  uint64_t frequency;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  return frequency;
}

#ifdef ROOT_TEST_BUDGET
/* Only root_budget_test.elf selects this policy. The normal root.elf always
 * reaches root_restart_decide, with exactly its previous budget and trace. */
static root_budget_policy root_test_budget;
static constexpr uint64_t root_test_leak_ms = 2000;
static constexpr unsigned int root_test_window_capacity = 1;
static constexpr unsigned int root_test_lifetime_limit = 3;
#endif

/* --- tiny dependency-free formatters (no libc/printf in the Root PD) --- */

static void put_dec(unsigned int v) {
  char buf[11];
  unsigned int i = sizeof(buf);
  buf[--i] = '\0';
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  microkit_dbg_puts(&buf[i]);
}

static void put_hex(seL4_Word v) {
  static const char hexdigits[] = "0123456789abcdef";
  char buf[2 + 16 + 1];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    buf[2 + i] = hexdigits[(v >> ((15 - i) * 4)) & 0xf];
  }
  buf[2 + 16] = '\0';
  microkit_dbg_puts(buf);
}

void init(void) {
  /* Reset every record EXPLICITLY rather than relying on it being .bss.
   * Root is the parent of every restartable driver and is not itself
   * restarted today, so the static zero would in fact do. The explicit loop is
   * here because a warm restart does not re-zero .bss (see the file header),
   * so "init() resets everything it relies on" is the invariant every PD in
   * this system is expected to hold; root should not be the exception that
   * teaches the wrong pattern. */
  for (size_t i = 0; i < ROOT_MAX_CHILDREN; i++) {
    child_records[i] = (root_child_record){};
  }
  const uint64_t frequency = root_clock_freq();
  if (root_clock_init(&root_clock_state, frequency, ROOT_CLOCK_MIN_HZ,
                      ROOT_CLOCK_MAX_HZ) != root_clock_ok ||
      root_clock_observe(&root_clock_state, root_now_ticks()) !=
          root_clock_ok) {
    microkit_dbg_puts("ROOT|clock|unavailable\n");
  } else {
    microkit_dbg_puts("ROOT|clock|freq=");
    put_hex(frequency);
    microkit_dbg_puts("\n");
  }
#ifdef ROOT_TEST_BUDGET
  uint64_t interval = 0;
  if (root_clock_interval(&root_clock_state, root_test_leak_ms, &interval) ==
      root_clock_ok) {
    root_test_budget = (root_budget_policy){
        .capacity = root_test_window_capacity,
        .lifetime_limit = root_test_lifetime_limit,
        .leak_interval_ticks = interval,
    };
  } else {
    root_test_budget = (root_budget_policy){};
  }
#endif
  microkit_dbg_puts("ROOT|init|budget=");
  put_dec(ROOT_RESTART_BUDGET);
  microkit_dbg_puts("|beam-budget=");
  put_dec(ROOT_BEAM_RESTART_BUDGET);
  microkit_dbg_puts("|entry=");
  put_hex((seL4_Word)restart_entry);
  microkit_dbg_puts("|beam-entry=");
  put_hex((seL4_Word)beam_reset_entry);
  microkit_dbg_puts("\n");
}

/*
 * A request for a child Root has already given up on. Logged so a test or an
 * operator can see the request arrived, and nothing else: the child stays
 * stopped and its dependents are not told a second time.
 */
static void root_log_ignored(root_child child, const char *request) {
  microkit_dbg_puts("ROOT|gone|child=");
  put_dec(child.value);
  microkit_dbg_puts("|ignored=");
  microkit_dbg_puts(request);
  microkit_dbg_puts("\n");
}

/*
 * Stop a child permanently and tell whoever depended on it.
 *
 * The record is marked gone before anything else, so any path that reaches
 * this child afterwards (a queued fault, a debug request) takes the ignored
 * branch instead of stopping or notifying again.
 *
 * The stop comes next and the notification after it, so a dependent can never
 * observe "gone" while the child is still briefly running and able to publish
 * a state change that would contradict it.
 *
 * Logging is last because it is the least important of the three: the console
 * is a debug-kernel affordance, and on a release build microkit_dbg_puts
 * compiles away entirely. The notification is what the running system acts on,
 * so it must not sit behind anything that can disappear.
 *
 * microkit_pd_stop() is TERMINAL here: nothing in this image brings the child
 * back afterwards, which is the behaviour blk-giveup-smoke pins. Microkit 2.3.0
 * adds microkit_pd_resume(), the primitive an operator-triggered revive would
 * be built on, and that is the concrete thing the 2.3.0 bump unblocks. Wiring
 * it is deliberately follow-up work rather than part of the version bump, and
 * it would have to clear the gone state too.
 *
 * The notify below is the IMMEDIATE microkit_notify rather than the deferred
 * form, and that is deliberate. microkit_deferred_notify has a single pending
 * slot (microkit_have_signal / microkit_signal_cap) shared by the whole PD, and
 * the signal only leaves on the reply that ends the current event. Since
 * root_giveup runs from the fault handler, switching to it would mean reasoning
 * about that reply path and about what else might already be holding the slot.
 * Note this is NOT a Microkit version question: the deferred machinery is
 * byte-identical between the 2.2.0 and 2.3.0 SDK headers, so a bump neither
 * enables nor blocks the change.
 */
static void root_giveup(root_child child, const char *reason) {
  root_record_give_up(&child_records[child.value]);
  microkit_pd_stop(child.value);

  const root_channel gone = root_gone_channel(child);
  if (gone.value != ROOT_GONE_CH_NONE) {
    microkit_notify(gone.value);
  }

  microkit_dbg_puts("ROOT|giveup|child=");
  put_dec(child.value);
  microkit_dbg_puts("|reason=");
  microkit_dbg_puts(reason);
  microkit_dbg_puts("\n");
}

/*
 * Apply the restart policy to one child: either restart it to a clean entry,
 * give up and stop it, or ignore a child that is already gone. Returns
 * nothing, because neither caller has anything to decide afterwards; the
 * outcome is reported entirely through the log.
 *
 * Shared by BOTH the fault path (fault(), a child crashed) and the debug path
 * (notified(), a test asked for a restart) so that a single budget governs the
 * two. That matters: if the debug path had its own budget, a test could restart
 * a driver more times than a genuinely faulting one ever could, and would then
 * be exercising a recovery path production can never reach.
 *
 * `request` names the caller ("restart" / "debug-restart") and is the only
 * thing distinguishing the two in the console output, which is what the
 * integration tests key on.
 *
 * The decision runs its checks in a fixed order (root_restart_decide):
 *
 * Gone: Root already stopped this child and told its dependents. Doing either
 * again, or resuming it, would contradict what they were told.
 *
 * Budget exhausted: this child has already spent its whole allowance and is
 * evidently not recovering. This is the reliability talk's "giving up"
 * decision, and it keeps one sick driver from livelocking the system: a
 * stopped driver degrades the service it provides, an endlessly restarting
 * one burns CPU at a priority above every client.
 *
 * No entry point: the image was assembled without one (an un-patched
 * .restart_config, so beam_reset_entry is still 0). Resuming at address 0
 * would fault instantly and burn the whole budget doing it.
 *
 * The restart itself is microkit_pd_restart(child, entry): it rewrites the
 * child's PC and resumes it, so the child re-runs _start -> main -> init() (or
 * _reset first, for beam_server). It does NOT re-zero .bss or reload .data
 * (that happens once, at boot, in the Microkit loader), which is why each
 * driver's init() has to be idempotent.
 */
static void root_restart_child(root_child child, const char *request) {
  root_child_record *record = &child_records[child.value];
  const seL4_Word entry = root_restart_entry(child);
  const unsigned int legacy_budget = root_restart_budget(child);
  if (root_clock_state.available &&
      root_clock_observe(&root_clock_state, root_now_ticks()) !=
          root_clock_ok) {
    microkit_dbg_puts("ROOT|clock|unavailable\n");
  }
  root_restart_action action;
  bool timed_charge = false;
#ifdef ROOT_TEST_BUDGET
  if (root_clock_state.available && root_test_budget.capacity != 0) {
    const root_timed_decision decision = root_restart_decide_at(
        record, &root_test_budget, entry, root_clock_state.last_ticks);
    if (decision.error == root_timed_valid) {
      action = decision.action;
      if (action == root_restart_resume) {
        *record = decision.next;
        timed_charge = true;
      }
    } else {
      root_clock_state.available = false;
      microkit_dbg_puts("ROOT|policy|invalid\n");
      action = root_restart_giveup_budget_exhausted;
    }
  } else {
    /* A clock unavailable at boot or lost later selects a lifetime-only
     * ceiling. It cannot grant a time-based refill. Keep the smaller of the
     * current image budget and this test policy's lifetime ceiling. */
    const unsigned int ceiling = legacy_budget < root_test_lifetime_limit
                                     ? legacy_budget
                                     : root_test_lifetime_limit;
    action = root_restart_decide(record, ceiling, entry);
  }
#else
  action = root_restart_decide(record, legacy_budget, entry);
#endif
  switch (action) {
  case root_restart_ignore_gone:
    root_log_ignored(child, request);
    return;
  case root_restart_giveup_budget_exhausted:
  case root_restart_giveup_window_exhausted:
  case root_restart_giveup_lifetime_exhausted:
  case root_restart_giveup_no_entry:
    root_giveup(child, root_restart_giveup_reason(action));
    return;
  case root_restart_resume:
    break;
  }

  /* Budget remains: spend one and restart. The count is incremented BEFORE the
   * restart so that if the child faults again immediately (the crasher PD
   * does exactly this, re-faulting inside init()), the re-entrant fault()
   * observes the already-charged count and the budget still converges. */
  if (!timed_charge) {
    root_record_charge(record);
  }
  microkit_pd_restart(child.value, entry);
  microkit_dbg_puts("ROOT|");
  microkit_dbg_puts(request);
  microkit_dbg_puts("|child=");
  put_dec(child.value);
  microkit_dbg_puts("|count=");
  put_dec(record->lifetime_count);
#ifdef ROOT_TEST_BUDGET
  microkit_dbg_puts("|window=");
  put_dec(record->window_count);
#endif
  microkit_dbg_puts("\n");
}

/*
 * Notification entry point. The ONLY channels ever wired to root are the
 * test-only healthy-restart and fault-injection groups generated by gen-sdf
 * --with-restart-debug, so in a production image root has no channels at all
 * and this function never runs.
 *
 * The signalling channel *is* the request: a Microkit notification carries no
 * payload, so rather than a shared word naming the target driver and operation,
 * system.zig wires one restart and one fault channel per restartable class.
 */
void notified(microkit_channel ch) {
  size_t index = 0;
  switch (
      root_notify_route((root_channel){.value = ch}, &root_channels, &index)) {
  case root_notify_debug_restart:
    /* A debug-restart request for a known driver class. Unlike fault(),
     * nothing has gone wrong here: a test is asking us to restart a HEALTHY
     * driver so it can isolate the recovery behavior. This path deliberately
     * involves no fault at all. */
    root_restart_child(root_class_child[index], "debug-restart");
    return;

  case root_notify_fault_inject: {
    /* Resume the real driver at an unmapped instruction address. Do not spend
     * a budget slot here: the resulting seL4 fault enters fault(), and that
     * path charges exactly one restart through root_restart_child(). Logging
     * precedes the restart because the child may fault as soon as it is
     * resumed.
     *
     * microkit_pd_restart also RESUMES the TCB, so a gone child is refused
     * here: injecting a fault into it would bring a stopped driver back to
     * life for one instruction and charge a budget that is already spent. */
    const root_child child = root_class_child[index];
    if (child_records[child.value].state == root_child_gone) {
      root_log_ignored(child, "fault-inject");
      return;
    }
    microkit_dbg_puts("ROOT|fault-inject|child=");
    put_dec(child.value);
    microkit_dbg_puts("\n");
    microkit_pd_restart(child.value, 0);
    return;
  }

  case root_notify_unexpected:
    break;
  }

  /* Anything else is a wiring bug: some PD holds a notification cap to root
   * that this code does not know about. Log it rather than silently ignoring
   * it, since a channel id we do not handle means a request that will never be
   * serviced, and the sender may well be waiting on the effect. */
  microkit_dbg_puts("ROOT|notify|unexpected-channel=");
  put_dec(ch);
  microkit_dbg_puts("\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
  (void)ch;
  (void)msginfo;
  return microkit_msginfo_new(0, 0);
}

/*
 * Called by libmicrokit when a child PD faults. `msginfo`'s label is the seL4
 * fault type; the message registers carry the fault detail (PC, address, FSR,
 * ...), whose layout depends on the fault type. We log a compact, parseable
 * record and the first couple of registers for triage, then decide.
 *
 * We resume the child ourselves via microkit_pd_restart, so we return
 * seL4_False to tell libmicrokit NOT to reply-to-resume the faulting thread.
 */
seL4_Bool fault(microkit_child raw_child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo) {
  (void)reply_msginfo;

  seL4_Word label = microkit_msginfo_get_label(msginfo);
  seL4_Word count = microkit_msginfo_get_count(msginfo);

  /* Log the fault before deciding anything, so a triage record survives even
   * if the restart below wedges. Format is deliberately compact and
   * machine-parseable: the integration tests grep these exact fields. */
  microkit_dbg_puts("ROOT|fault|child=");
  put_dec(raw_child);
  microkit_dbg_puts("|label=");
  put_hex(label);

  /* The fault detail lives in the message registers, but HOW MANY are valid
   * depends on the fault type (a VM fault carries PC/addr/FSR, a cap fault
   * carries different words). Rather than decode every seL4 fault layout in
   * the error kernel, dump the first two registers when the message says they
   * are present. `count` is the message length, so these two guards are just
   * bounds checks against reading registers the sender never set. Two words is
   * enough to identify the faulting PC and address for every fault type we
   * have actually hit. */
  if (count >= 1) {
    microkit_dbg_puts("|mr0=");
    put_hex(microkit_mr_get(0));
  }
  if (count >= 2) {
    microkit_dbg_puts("|mr1=");
    put_hex(microkit_mr_get(1));
  }
  microkit_dbg_puts("\n");

  /* A child id the ABI does not declare has no budget slot, and
   * BASE_TCB_CAP + id may not be its TCB (or any TCB), so no capability is
   * invoked for it: no restart, no stop. Returning seL4_False sends no reply,
   * so the faulting thread stays blocked on its fault and never runs again.
   * In practice this is unreachable, because only children carry a fault
   * badge; it exists so a future topology change fails visibly and safely. */
  root_child child = {};
  if (!root_child_from_raw(raw_child, ROOT_MAX_CHILDREN, root_known_children,
                           &child)) {
    microkit_dbg_puts("ROOT|reject|child=");
    put_dec(raw_child);
    microkit_dbg_puts("|reason=out-of-range\n");
    return seL4_False;
  }

  /* Apply the restart policy. Identical to what a debug-restart request gets,
   * and sharing one budget between the two (see root_restart_child). */
  root_restart_child(child, "restart");

  /* seL4_False tells libmicrokit NOT to reply to the fault IPC. Replying is
   * the other way to resume a faulting thread (it restarts it at the faulting
   * instruction, which for a driver that just dereferenced NULL would simply
   * fault again). We have already resumed the child ourselves at a clean
   * entry, or stopped it, or left a gone child stopped, so there is nothing
   * left for libmicrokit to do. */
  return seL4_False;
}
