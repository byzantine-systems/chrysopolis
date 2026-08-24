/*
 * root.c - Chrysopolis Root fault-handler / process-manager PD.
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
 *     the mechanism lives in src/runtime/restart.c.
 */
#include <microkit.h>

/* Compile-time fallback child ELF entry point, matching every board's
 * microkit.ld shipped in the SDK. Our build never defines this macro: the value
 * cannot be threaded from a build-time-read ELF into a -D flag through Zig's
 * build graph, which is precisely why it is patched into a section instead. The
 * fallback only covers a by-hand `zig build` plus `microkit` run, where nothing
 * patches .restart_config. */
#ifndef MICROKIT_RESTART_ENTRY
#define MICROKIT_RESTART_ENTRY 0x200000
#endif

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
 *     memory and then enters the normal boot (see src/runtime/restart.c).
 *     modules/images.nix resolves that symbol with llvm-nm and patches it in.
 *
 * The initializers are the fallback for an un-patched build. A zero
 * beam_reset_entry means "not patched", which root treats as "beam_server is
 * not restartable in this image" rather than jumping to address 0. */
__attribute__((__section__(".restart_config"),
               used)) volatile seL4_Uint64 restart_config[2] = {
    MICROKIT_RESTART_ENTRY,
    0,
};

#define restart_entry (restart_config[0])
#define beam_reset_entry (restart_config[1])

/* Microkit child ids are small; size the table to the channel-id space. */
#define ROOT_MAX_CHILDREN 64

/*
 * Test-only debug-restart channels (present only in the restart image, which
 * tools/sdf/system.zig generates with --with-restart-debug; production SDFs
 * wire no channel to root at all, so these ids are simply never signalled).
 *
 * One channel per restartable driver class, because a Microkit notification
 * carries no payload: the channel the signal arrives on IS the request. The ids
 * are pinned in system.zig and map 1:1 onto the pinned child ids, letting a
 * test restart a HEALTHY driver on demand. Fault detection is already covered
 * by the crasher PD; what the driver-restart tests need to exercise is the
 * recovery path, so this deliberately does not involve a fault.
 *
 * Note these are microkit_channel ids, a SEPARATE id space from the
 * microkit_child ids they map to (BASE_OUTPUT_NOTIFICATION_CAP vs
 * BASE_TCB_CAP).
 */
#define ROOT_DEBUG_CH_SERIAL 0
#define ROOT_DEBUG_CH_TIMER 1
#define ROOT_DEBUG_CH_BLK 2
#define ROOT_DEBUG_CH_ETH 3
#define ROOT_DEBUG_CH_MAX ROOT_DEBUG_CH_ETH

/* Child ids, pinned in tools/sdf/system.zig (an ABI with this file). */
#define ROOT_CHILD_SERIAL 0
#define ROOT_CHILD_TIMER 1
#define ROOT_CHILD_BLK 2
#define ROOT_CHILD_ETH 3
#define ROOT_CHILD_BEAM 5

/*
 * Give-up notification channels, pinned in tools/sdf/system.zig.
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
#define ROOT_GONE_CH_BLK 10
#define ROOT_GONE_CH_NONE 0xff

/* Debug channel -> child id. Indexed by channel, so it must stay dense and in
 * ROOT_DEBUG_CH_* order. */
static const microkit_child debug_restart_child[ROOT_DEBUG_CH_MAX + 1] = {
    [ROOT_DEBUG_CH_SERIAL] = ROOT_CHILD_SERIAL,
    [ROOT_DEBUG_CH_TIMER] = ROOT_CHILD_TIMER,
    [ROOT_DEBUG_CH_BLK] = ROOT_CHILD_BLK,
    [ROOT_DEBUG_CH_ETH] = ROOT_CHILD_ETH,
};

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
static microkit_channel root_gone_channel(microkit_child child) {
  switch (child) {
  case ROOT_CHILD_BLK:
    return ROOT_GONE_CH_BLK;
  default:
    return ROOT_GONE_CH_NONE;
  }
}

/* Restart budget for a driver PD. After this many restarts we stop the child
 * rather than spin forever (the reliability talk's "giving up" decision). A
 * time-windowed budget (reset the count after the child stays up for a while)
 * is a future refinement: it needs a timer channel wired into the Root PD. */
#define ROOT_RESTART_BUDGET 8

/* beam_server's budget is larger because its restarts are not all failures. A
 * driver restart always means a driver went wrong, but a BEAM PD restart is
 * also what an ordinary `init:stop()` at the shell produces, and eight of those
 * should not permanently stop the system. The budget still exists: an ERTS that
 * faults on every boot is exactly the runaway this bounds. */
#define ROOT_BEAM_RESTART_BUDGET 64

/*
 * A switch rather than a table, for the same reason root_gone_channel above is
 * one: with a designated-initialiser array every unlisted child would default
 * to a budget of 0, which does not read as "unlisted", it reads as "give up on
 * the first fault". The default has to be the driver budget, and only a switch
 * makes that the default.
 */
static unsigned int root_restart_budget(microkit_child child) {
  switch (child) {
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
 * has no usable entry, which the caller treats as "cannot restart this one".
 */
static seL4_Word root_restart_entry(microkit_child child) {
  if (child == ROOT_CHILD_BEAM) {
    return (seL4_Word)beam_reset_entry;
  }
  return (seL4_Word)restart_entry;
}

static unsigned int restart_count[ROOT_MAX_CHILDREN];

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
  /* Zero the budget table EXPLICITLY rather than relying on it being .bss.
   * Root is the parent of every restartable driver and is not itself
   * restarted today, so the static zero would in fact do. The explicit loop is
   * here because a warm restart does not re-zero .bss (see the file header),
   * so "init() resets everything it relies on" is the invariant every PD in
   * this system is expected to hold; root should not be the exception that
   * teaches the wrong pattern. */
  for (unsigned int i = 0; i < ROOT_MAX_CHILDREN; i++) {
    restart_count[i] = 0;
  }
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
 * Apply the restart policy to one child: either restart it to a clean entry or
 * give up and stop it. Returns nothing, because neither caller has anything to
 * decide afterwards; the outcome is reported entirely through the log.
 *
 * Shared by BOTH the fault path (fault(), a child crashed) and the debug path
 * (notified(), a test asked for a restart) so that a single budget governs the
 * two. That matters: if the debug path had its own budget, a test could restart
 * a driver more times than a genuinely faulting one ever could, and would then
 * be exercising a recovery path production can never reach.
 *
 * `tag` is the log prefix ("ROOT|restart" / "ROOT|debug-restart") and is the
 * only thing distinguishing the two callers in the console output, which is
 * what the integration tests key on.
 *
 * The restart itself is microkit_pd_restart(child, restart_entry): it rewrites
 * the child's PC to the ELF entry point and resumes it, so the child re-runs
 * _start -> main -> init(). It does NOT re-zero .bss or reload .data (that
 * happens once, at boot, in the Microkit loader), which is why each driver's
 * init() has to be idempotent.
 */
/*
 * Stop a child permanently and tell whoever depended on it.
 *
 * The stop comes first and the notification second, so a dependent can never
 * observe "gone" while the child is still briefly running and able to publish a
 * state change that would contradict it.
 *
 * Logging is last because it is the least important of the three: the console
 * is a debug-kernel affordance, and on a release build microkit_dbg_puts
 * compiles away entirely. The notification is what the running system acts on,
 * so it must not sit behind anything that can disappear.
 */
static void root_giveup(microkit_child child, const char *reason) {
  microkit_pd_stop(child);

  microkit_channel gone = root_gone_channel(child);
  if (gone != ROOT_GONE_CH_NONE) {
    microkit_notify(gone);
  }

  microkit_dbg_puts("ROOT|giveup|child=");
  put_dec(child);
  microkit_dbg_puts("|reason=");
  microkit_dbg_puts(reason);
  microkit_dbg_puts("\n");
}

static void root_restart_child(microkit_child child, const char *tag) {
  /* Guard the restart_count[] index before touching it. A child id past the
   * table means we cannot account for its budget, and a child we cannot
   * account for could spin in a restart loop forever, so refuse to restart it
   * at all and stop it instead. In practice this is unreachable (child ids are
   * pinned small in tools/sdf/system.zig); it exists so a future topology
   * change fails loudly and safely rather than corrupting memory past the
   * array. */
  if (child >= ROOT_MAX_CHILDREN) {
    root_giveup(child, "out-of-range");
    return;
  }

  /* Budget exhausted: this child has already spent its whole allowance and is
   * evidently not recovering. Stop it rather than restart it forever. This is
   * the reliability talk's "giving up" decision, and it is what keeps one sick
   * driver from livelocking the system: a stopped driver degrades the service
   * it provides, an endlessly restarting one burns CPU at a priority above
   * every client. */
  if (restart_count[child] >= root_restart_budget(child)) {
    root_giveup(child, "budget-exhausted");
    return;
  }

  /* No entry point for this child means the image was assembled without one
   * (an un-patched .restart_config, so beam_reset_entry is still 0). Resuming
   * at address 0 would fault instantly and burn the whole budget doing it, so
   * stop the child instead and say why. */
  seL4_Word entry = root_restart_entry(child);
  if (entry == 0) {
    root_giveup(child, "no-restart-entry");
    return;
  }

  /* Budget remains: spend one and restart. The count is incremented BEFORE the
   * restart so that if the child faults again immediately (the crasher PD
   * does exactly this, re-faulting inside init()), the re-entrant fault()
   * observes the already-charged count and the budget still converges. */
  restart_count[child]++;
  microkit_pd_restart(child, entry);
  microkit_dbg_puts(tag);
  microkit_dbg_puts("|child=");
  put_dec(child);
  microkit_dbg_puts("|count=");
  put_dec(restart_count[child]);
  microkit_dbg_puts("\n");
}

/*
 * Notification entry point. The ONLY channels ever wired to root are the
 * test-only debug-restart ones (ROOT_DEBUG_CH_*, generated by gen-sdf
 * --with-restart-debug), so in a production image root has no channels at all
 * and this function never runs.
 *
 * The signalling channel *is* the request: a Microkit notification carries no
 * payload, so rather than a shared word naming the target driver, system.zig
 * wires one channel per restartable class and this maps channel -> child id.
 */
void notified(microkit_channel ch) {
  /* A debug-restart request for a known driver class. Unlike fault(), nothing
   * has gone wrong here: a test is asking us to restart a HEALTHY driver so it
   * can assert that the driver and its clients recover. Fault *detection* is
   * already covered by the crasher PD, so this path deliberately involves no
   * fault at all. */
  if (ch <= ROOT_DEBUG_CH_MAX) {
    root_restart_child(debug_restart_child[ch], "ROOT|debug-restart");
    return;
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
seL4_Bool fault(microkit_child child, microkit_msginfo msginfo,
                microkit_msginfo *reply_msginfo) {
  (void)reply_msginfo;

  seL4_Word label = microkit_msginfo_get_label(msginfo);
  seL4_Word count = microkit_msginfo_get_count(msginfo);

  /* Log the fault before deciding anything, so a triage record survives even
   * if the restart below wedges. Format is deliberately compact and
   * machine-parseable: the integration tests grep these exact fields. */
  microkit_dbg_puts("ROOT|fault|child=");
  put_dec(child);
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

  /* Apply the restart policy. Identical to what a debug-restart request gets,
   * and sharing one budget between the two (see root_restart_child). */
  root_restart_child(child, "ROOT|restart");

  /* seL4_False tells libmicrokit NOT to reply to the fault IPC. Replying is
   * the other way to resume a faulting thread (it restarts it at the faulting
   * instruction, which for a driver that just dereferenced NULL would simply
   * fault again). We have already resumed the child ourselves at a clean
   * entry, or stopped it, so there is nothing left for libmicrokit to do. */
  return seL4_False;
}
