/*
 * restart.c - warm-restart support for the beam_server PD.
 *
 * microkit_pd_restart(child, entry) writes exactly ONE register: the child's
 * PC (seL4_TCB_WriteRegisters with count 1). It does not re-zero .bss, does not
 * reload .data, and does not reset SP. The Microkit loader does those things
 * once, at boot, and never again.
 *
 * A driver PD survives that treatment because its entire mutable state is a
 * handful of .bss words its init() rewrites anyway. beam_server does not:
 * beam_test.elf carries ~192 KiB of file-backed writable data and ~28 MiB of
 * .bss (ERTS globals, the libc syscall table, libmicrokitco's controller).
 * Re-entering _start over that memory finds libc_define_syscall's slots already
 * claimed, ERTS's already-initialised guards already set, and the allocator
 * pointing at its own leftovers. So beam_server has to reset its own memory
 * before it can be restarted, and this file is that reset.
 *
 * Microkit has no memory-reset facility (the v2.3.0-dev manual still documents
 * microkit_pd_restart as PC-only, and there is no restart attribute on
 * <protection_domain>), and neither does LionsOS. rust-sel4's crates/sel4-reset
 * does solve exactly this, and the structure here is taken from it:
 *
 *   - a dedicated reset entry point in asm, separate from _start;
 *   - it switches SP to a stack the reset itself does not touch, before it
 *     touches anything;
 *   - it restores the writable segment, then branches into the normal boot;
 *   - memory that must survive a reset is opted OUT of the restored range
 *     (sel4-reset calls that a .persistent section; here it is the snapshot
 *     region itself, which is a separate MR and so is never in range).
 *
 * sel4-reset sources its pristine copy from the ELF: it adds a second,
 * read-only PT_LOAD aliasing the same file bytes as the writable segment, and
 * restores from that (copy the file-backed part, zero-fill the rest).
 *
 * That cannot work here, because the Microkit tool patches symbols into the
 * LOADED IMAGE and never into the ELF file. tool/microkit/src/elf.rs reads each
 * segment into a buffer expanded to p_memsz and write_symbol() patches that
 * buffer, so an alias of the file bytes would be missing every patched value.
 * In our link those values sit in .bss: beam_heap_start and beam_snapshot_start
 * (setvar_vaddr), plus libmicrokit's own microkit_name, microkit_irqs,
 * microkit_notifications, microkit_pps and microkit_passive. A zero-fill would
 * wipe all of them. sel4-reset would put them in .persistent; we cannot,
 * because they are defined inside libmicrokit.a's main.o.
 *
 * So the pristine copy is taken at runtime instead, at the first instruction of
 * the first boot, when the loaded image is pristine BY DEFINITION:
 *
 *   - [__init_array_start, _bss) is copied verbatim into the snapshot region.
 *     That covers .init_array, .data.rel.ro, .got, .data and the orphan
 *     per-variable .data and sDDF config sections the linker places after it.
 *   - [_bss, _bss_end) is NOT copied. At that instant .bss is zero everywhere
 *     the Microkit tool did not patch it, so a scan for non-zero runs
 *     DISCOVERS the set of values that must survive a reset, instead of us
 *     keeping a list of them in sync by hand. Those runs are recorded and
 *     replayed after the zero-fill.
 *
 * The discovered set is expected to be around 120 bytes across half a dozen
 * runs. beam_restart_report() prints them at cold boot precisely so that a
 * surprise entry is visible rather than silent: a run that does not correspond
 * to a symbol we expect the tool to patch means an assumption in this comment
 * is wrong.
 *
 * The restart request usually arrives from bringup_exit(), which ERTS reaches
 * on a COTHREAD stack allocated out of beam_heap. Microkit leaves SP alone
 * across a restart, so the reset trampoline would otherwise be running on a
 * stack inside the very arena libc_init is about to re-hand-out. It switches to
 * a stack inside the snapshot region before it calls any C.
 */
#include <microkit.h>
#include <sel4/sel4.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Stringify, so the reset trampoline's stack offset is written once as a C
 * constant and reused as an assembly immediate rather than duplicated.
 * BEAM_RESET_STACK_TOP_STR wraps the expansion in an object-like macro on
 * purpose: a function-like macro call inside the asm string literal makes
 * clang-format cascade the whole trampoline into unreadable indentation. */
#define BEAM_STR_(x) #x
#define BEAM_STR(x) BEAM_STR_(x)
#define BEAM_RESET_STACK_TOP_STR BEAM_STR(BEAM_RESET_STACK_TOP)

/*
 * Snapshot region layout. Offsets are fixed constants rather than a struct with
 * padding because the reset trampoline below has to compute the stack top in
 * assembly, where offsetof() is not available.
 *
 * BEAM_SNAPSHOT_SIZE is an ABI with tools/sdf/system.zig, which creates the
 * matching memory region; modules/images.nix asserts at build time that the
 * data area is big enough for this ELF's writable segment.
 */
#define BEAM_SNAPSHOT_SIZE 0x80000 /* 512 KiB; matches tools/sdf/system.zig */

/* Written without a `u` suffix: BEAM_RESET_STACK_TOP is stringified into the
 * reset trampoline's assembly, where a C integer suffix would not assemble. */
#define BEAM_RESET_STACK_OFF 0x1000
#define BEAM_RESET_STACK_SIZE 0x4000
#define BEAM_RESET_STACK_TOP 0x5000
#define BEAM_SURVIVORS_OFF 0x5000
#define BEAM_SURVIVORS_SIZE 0x1000
#define BEAM_DATA_OFF 0x6000
#define BEAM_DATA_SIZE (BEAM_SNAPSHOT_SIZE - BEAM_DATA_OFF)

_Static_assert(BEAM_RESET_STACK_TOP ==
                   BEAM_RESET_STACK_OFF + BEAM_RESET_STACK_SIZE,
               "reset stack top must be the end of the reset stack");
_Static_assert(BEAM_SURVIVORS_OFF >= BEAM_RESET_STACK_TOP,
               "the survivor area must not overlap the reset stack");
_Static_assert(BEAM_DATA_OFF >= BEAM_SURVIVORS_OFF + BEAM_SURVIVORS_SIZE,
               "the data area must not overlap the survivor area");

/* Written last on the cold-boot path, so a torn capture reads as "cold" and is
 * simply retaken rather than half-restored. seL4 zeroes fresh frames, so an
 * untouched region reads 0 here, which is what makes "no magic" a reliable
 * cold-boot test with nothing to initialise first. */
#define BEAM_SNAPSHOT_MAGIC 0x43485259534E5031ull /* "CHRYSNP1" */

typedef struct {
  uint64_t magic;
  uint64_t generation;     /* 1 on cold boot, incremented by every reset */
  uint64_t saved_sp;       /* the cold-boot SP, so a reset can restore it */
  uint64_t data_bytes;     /* used length of the data area */
  uint64_t survivor_bytes; /* used length of the survivor area */
} beam_snapshot_hdr_t;

/* One discovered non-zero run in .bss, followed in the survivor area by `len`
 * bytes of payload padded up to 8. */
typedef struct {
  uint32_t offset; /* from _bss */
  uint32_t len;
} beam_survivor_hdr_t;

/*
 * Zero words this many apart or closer are kept inside one run rather than
 * splitting it. Purely an encoding choice: a struct with interior zero fields
 * (microkit_name[64] is mostly padding, and several of the patched words are
 * bitmasks with zero halves) would otherwise become a dozen tiny runs. Over-
 * capturing a few zero bytes is harmless, since they are being restored to the
 * value the zero-fill would have given them anyway.
 */
#define BEAM_SURVIVOR_GAP_WORDS 8u

/*
 * The snapshot region base, patched by the Microkit tool at synthesis time
 * (setvar_vaddr="beam_snapshot_start" in tools/sdf/system.zig), exactly like
 * beam_heap_start in main.c.
 *
 * This lives in .bss, which means it is itself one of the survivors the scan
 * discovers. The ordering keeps that safe: the reset trampoline reads it BEFORE
 * the zero-fill and keeps the pointer in a register/stack local, and the replay
 * then puts it back.
 */
uintptr_t beam_snapshot_start;

/* Writable-segment bounds from the board's microkit.ld. __init_array_start is
 * PROVIDEd there and is the first section in the data PHDR; _bss/_bss_end
 * bracket .bss. These are addresses, not objects: taking their address is the
 * only correct way to read them, and none of them is storage the reset can
 * clobber. */
extern char __init_array_start[];
extern char _bss[];
extern char _bss_end[];

static inline uint8_t *snapshot_base(void) {
  return (uint8_t *)beam_snapshot_start;
}

static inline beam_snapshot_hdr_t *snapshot_hdr(uint8_t *base) {
  return (beam_snapshot_hdr_t *)base;
}

/* ---- tiny dependency-free formatters (the console is not up yet) ---- */

static void put_dec(uint64_t v) {
  char buf[21];
  unsigned int i = sizeof(buf);
  buf[--i] = '\0';
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  microkit_dbg_puts(&buf[i]);
}

static void put_hex(uint64_t v) {
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

/*
 * Unrecoverable: we cannot produce a pristine image, so we must not pretend to.
 * Failing loudly here turns a capacity problem into an early-boot stop instead
 * of memory corruption thousands of instructions later.
 *
 * This parks, which is what this issue removed from bringup_exit, because a
 * restart cannot help here: it re-runs the same code over the same too-small
 * region and fails identically, so spending root's budget on it would replace a
 * legible stop with a restart loop. modules/images.nix asserts these same
 * bounds at build time, so reaching this means the ELF and the image were built
 * inconsistently.
 */
static void restart_panic(const char *why) {
  microkit_dbg_puts("BEAM|restart|FATAL|");
  microkit_dbg_puts(why);
  microkit_dbg_puts("\n");
  for (;;) {
    seL4_Yield();
  }
}

/*
 * Record every non-zero run in [_bss, _bss_end) into the survivor area.
 *
 * Scanned a word at a time: .bss is tens of megabytes and all but a few dozen
 * bytes of it are zero, so a byte-wise scan would spend most of a second under
 * TCG for no extra precision (runs are recorded at word granularity anyway).
 * The tail bytes below the last whole word are checked separately so a .bss
 * whose length is not a multiple of 8 cannot hide a patched byte at the end.
 */
static uint64_t capture_survivors(uint8_t *base) {
  const uint64_t *words = (const uint64_t *)(void *)_bss;
  size_t total = (size_t)(_bss_end - _bss);
  size_t nwords = total / sizeof(uint64_t);
  uint8_t *out = base + BEAM_SURVIVORS_OFF;
  size_t used = 0;

  for (size_t i = 0; i < nwords;) {
    if (words[i] == 0) {
      i++;
      continue;
    }

    /* Extend the run while the next non-zero word is within the coalescing
     * gap, so one patched struct stays one entry. */
    size_t start = i;
    size_t last = i;
    i++;
    while (i < nwords && (i - last) <= BEAM_SURVIVOR_GAP_WORDS) {
      if (words[i] != 0) {
        last = i;
      }
      i++;
    }

    size_t off = start * sizeof(uint64_t);
    size_t len = (last - start + 1) * sizeof(uint64_t);
    if (used + sizeof(beam_survivor_hdr_t) + len > BEAM_SURVIVORS_SIZE) {
      restart_panic("survivor-area-overflow");
    }

    beam_survivor_hdr_t hdr = {.offset = (uint32_t)off, .len = (uint32_t)len};
    memcpy(out + used, &hdr, sizeof(hdr));
    memcpy(out + used + sizeof(hdr), _bss + off, len);
    used += sizeof(hdr) + len;
  }

  /* The sub-word tail. .bss is ALIGN(4)-terminated by microkit.ld, so this is
   * at most four bytes, but a patched value ending there would be lost without
   * it and the failure would look like a random symbol coming back zero. */
  size_t tail_off = nwords * sizeof(uint64_t);
  if (tail_off < total) {
    size_t tail_len = total - tail_off;
    bool nonzero = false;
    for (size_t i = 0; i < tail_len; i++) {
      if (((const uint8_t *)_bss)[tail_off + i] != 0) {
        nonzero = true;
        break;
      }
    }
    if (nonzero) {
      if (used + sizeof(beam_survivor_hdr_t) + tail_len > BEAM_SURVIVORS_SIZE) {
        restart_panic("survivor-area-overflow");
      }
      beam_survivor_hdr_t hdr = {.offset = (uint32_t)tail_off,
                                 .len = (uint32_t)tail_len};
      memcpy(out + used, &hdr, sizeof(hdr));
      memcpy(out + used + sizeof(hdr), _bss + tail_off, tail_len);
      used += sizeof(hdr) + tail_len;
    }
  }

  return used;
}

/*
 * Cold-boot capture, called from _start with the incoming SP.
 *
 * Runs before libmicrokit's main(), therefore before __init_array and before
 * init(), which is the only window in which the writable segment is guaranteed
 * to be exactly what the loader and the Microkit tool put there. Anything later
 * (an .init_array constructor, the top of init()) would snapshot state some
 * constructor had already mutated.
 *
 * Called on the warm path too, where it does nothing: _reset has already
 * restored the image and the magic is set. Keeping the check here rather than
 * relying on the two entry points staying distinct means a future change that
 * routes a reset through _start still cannot double-capture.
 */
void beam_boot_capture(uintptr_t sp) {
  uint8_t *base = snapshot_base();
  if (base == NULL) {
    /* The SDF did not map the region, so no restart is possible. Say so once;
     * a cold boot still works, and root will simply never get a live child
     * back if this instance dies. */
    microkit_dbg_puts("BEAM|restart|unavailable|no-snapshot-region\n");
    return;
  }

  beam_snapshot_hdr_t *hdr = snapshot_hdr(base);
  if (hdr->magic == BEAM_SNAPSHOT_MAGIC) {
    return;
  }

  size_t data_len = (size_t)(_bss - __init_array_start);
  if (data_len > BEAM_DATA_SIZE) {
    restart_panic("data-area-overflow");
  }
  memcpy(base + BEAM_DATA_OFF, __init_array_start, data_len);

  hdr->saved_sp = sp;
  hdr->data_bytes = data_len;
  hdr->survivor_bytes = capture_survivors(base);
  hdr->generation = 1;
  hdr->magic = BEAM_SNAPSHOT_MAGIC;
}

/*
 * Warm-restart restore, called from _reset while already on the reset stack.
 * Returns the SP the cold boot ran on, which the trampoline installs before
 * branching to main().
 *
 * The ordering is the whole point of this function:
 *
 *   1. Read everything we need out of .bss FIRST (the snapshot pointer). After
 *      step 3 that global is zero, and a re-read would dereference NULL.
 *   2. Restore the file-backed part of the writable segment.
 *   3. Zero .bss, which is what the Microkit loader did at boot.
 *   4. Replay the discovered survivors, putting the Microkit tool's patches
 *      back on top of the zeros.
 *
 * Locals live on the reset stack inside the snapshot region, which is a
 * separate MR and therefore not in any restored range. memcpy/memset come from
 * musl and are pure code with no writable global state, so they are safe to
 * call while the image is mid-restore; nothing else here may be, which is why
 * there is no logging in this function.
 */
uintptr_t beam_reset_restore(void) {
  uint8_t *base = snapshot_base();
  beam_snapshot_hdr_t *hdr = snapshot_hdr(base);

  if (hdr->magic != BEAM_SNAPSHOT_MAGIC) {
    restart_panic("reset-without-snapshot");
  }

  uintptr_t sp = (uintptr_t)hdr->saved_sp;
  size_t data_len = (size_t)hdr->data_bytes;
  size_t survivor_len = (size_t)hdr->survivor_bytes;

  memcpy(__init_array_start, base + BEAM_DATA_OFF, data_len);
  memset(_bss, 0, (size_t)(_bss_end - _bss));

  const uint8_t *in = base + BEAM_SURVIVORS_OFF;
  for (size_t used = 0; used + sizeof(beam_survivor_hdr_t) <= survivor_len;) {
    beam_survivor_hdr_t run;
    memcpy(&run, in + used, sizeof(run));
    memcpy(_bss + run.offset, in + used + sizeof(run), run.len);
    used += sizeof(run) + run.len;
  }

  hdr->generation++;
  return sp;
}

/* True once the PD has been restarted at least once. main.c gates the
 * shared-ring reconciles on this: the peers (virtualisers, copier, fatfs) kept
 * running across the restart and their rings still hold the dead instance's
 * state, which a cold boot never has to deal with. */
bool beam_warm_start(void) {
  uint8_t *base = snapshot_base();
  if (base == NULL) {
    return false;
  }
  beam_snapshot_hdr_t *hdr = snapshot_hdr(base);
  return hdr->magic == BEAM_SNAPSHOT_MAGIC && hdr->generation > 1;
}

/*
 * Counts entries into init(). An ordinary .bss counter, which puts it INSIDE
 * the range beam_reset_restore zeroes, so it must read 1 on every boot, warm or
 * cold. The snapshot header's generation is its counterpart: that lives in the
 * snapshot region, outside every restored range, so it climbs.
 *
 * The pair is what separates "the PD restarted" from "the PD restarted and
 * forgot everything" on the console, and beam-restart-smoke asserts both
 * halves. A bss_boots above 1 means the reset did not happen. Compare the
 * crasher PD, where a climbing .bss counter is exactly what proves it
 * re-executed, because it has no reset to undo it.
 */
static unsigned int bss_boots;

static void beam_restart_report(void);

void beam_boot_banner(void) {
  uint8_t *base = snapshot_base();

  bss_boots++;
  microkit_dbg_puts("BEAM|boot|generation=");
  put_dec(base == NULL ? 0 : snapshot_hdr(base)->generation);
  microkit_dbg_puts("|bss-counter=");
  put_dec(bss_boots);
  microkit_dbg_puts("\n");

  if (!beam_warm_start()) {
    beam_restart_report();
  }
}

/*
 * Print the discovered survivor runs. Called once from init() on the cold boot.
 *
 * The survivor scan replaces a hand-maintained list of Microkit-patched .bss
 * symbols with a discovery rule, and the rule is only as good as the claim that
 * .bss is otherwise zero at _start. Printing what was found is how that claim
 * stays checkable: the expected runs resolve to
 * microkit_name, microkit_irqs/notifications/pps/passive, beam_heap_start and
 * beam_snapshot_start, and anything else is a signal that something writes .bss
 * before _start runs.
 */
static void beam_restart_report(void) {
  uint8_t *base = snapshot_base();
  if (base == NULL) {
    return;
  }
  beam_snapshot_hdr_t *hdr = snapshot_hdr(base);

  microkit_dbg_puts("BEAM|snapshot|data=");
  put_dec(hdr->data_bytes);
  microkit_dbg_puts("|survivor-bytes=");
  put_dec(hdr->survivor_bytes);
  microkit_dbg_puts("|bss=");
  put_dec((uint64_t)(_bss_end - _bss));
  microkit_dbg_puts("\n");

  const uint8_t *in = base + BEAM_SURVIVORS_OFF;
  size_t survivor_len = (size_t)hdr->survivor_bytes;
  for (size_t used = 0; used + sizeof(beam_survivor_hdr_t) <= survivor_len;) {
    beam_survivor_hdr_t run;
    memcpy(&run, in + used, sizeof(run));
    microkit_dbg_puts("BEAM|snapshot|survivor|vaddr=");
    put_hex((uint64_t)(uintptr_t)(_bss + run.offset));
    microkit_dbg_puts("|len=");
    put_dec(run.len);
    microkit_dbg_puts("\n");
    used += sizeof(run) + run.len;
  }
}

/*
 * Ask root to restart this PD, carrying `status` to it.
 *
 * A Microkit PD cannot exit, and a notification carries no payload, so the
 * request is made by faulting DELIBERATELY at a reserved unmapped address whose
 * low byte is the exit code. Root's fault() already logs mr0/mr1, and for a VM
 * fault mr1 is the faulting address, so the code arrives at the error kernel
 * with no channel, no SDF surface and nothing for production-sdf-gate to
 * police.
 *
 * The deeper reason to prefer a fault over a channel: this is the same path a
 * genuine ERTS segfault takes. A controlled exit and a crash therefore exercise
 * one recovery mechanism rather than two, and the one they exercise is the one
 * that has to work when nothing is controlled.
 *
 * BEAM_EXIT_FAULT_BASE must be unmapped in beam_server's VSpace, which
 * modules/images.nix asserts against the Microkit tool's own report.txt, so a
 * future memory region cannot quietly turn this fault into a store.
 *
 * The trailing __builtin_trap() is unreachable if the store faults, which it
 * does. It is here so that a build where the address HAS become mapped stops
 * anyway (brk, which root also sees as a fault) instead of running on with
 * ERTS half torn down.
 */
#define BEAM_EXIT_FAULT_BASE 0xBEA00000u

_Noreturn void beam_request_restart(int status) {
  /* microkit_dbg_puts, not printf: this is a direct kernel putchar, whereas
   * printf enqueues into the serial TX ring that the driver drains later. We
   * are about to fault, so a queued line might never be drained, and the line
   * announcing why the PD died is the one that must not be lost. */
  microkit_dbg_puts("BEAM|exit|code=");
  put_dec((uint64_t)(unsigned int)status);
  microkit_dbg_puts("|requesting-restart\n");

  *(volatile uint8_t *)(uintptr_t)(BEAM_EXIT_FAULT_BASE +
                                   (unsigned int)(status & 0xff)) = 0;
  __builtin_trap();
}

/*
 * The two entry points, in assembly because both run with no usable stack (the
 * reset one) or before anything at all has been set up (the boot one).
 *
 * _start replaces libmicrokit's crt0.o. That object defines only _start and
 * references only main, so defining _start here means the linker never extracts
 * it from libmicrokit.a (which the beam link pulls lazily), and there is no
 * duplicate symbol. It stays in .text.start so microkit.ld places it first and
 * e_entry remains 0x200000, which is what modules/images.nix asserts is uniform
 * across every child of root.
 *
 * _reset is what root branches beam_server to instead of _start. Its address is
 * read out of the linked ELF by modules/images.nix and patched into root's
 * .restart_config, the same way the shared child entry point already is.
 *
 * beam_snapshot_start is addressed with adrp/add rather than a literal-pool
 * load, so neither entry point depends on a literal pool being reachable from
 * its own section.
 */
__asm__(".section .text.start,\"ax\",%progbits\n"
        ".global _start\n"
        ".type _start, %function\n"
        "_start:\n"
        "    mov  x0, sp\n"
        "    bl   beam_boot_capture\n"
        "    b    main\n"
        ".size _start, . - _start\n");

__asm__(
    ".section .text.reset,\"ax\",%progbits\n"
    ".global _reset\n"
    ".type _reset, %function\n"
    "_reset:\n"
    /* Snapshot base, read before the restore zeroes the global holding it. */
    "    adrp x9, beam_snapshot_start\n"
    "    add  x9, x9, :lo12:beam_snapshot_start\n"
    "    ldr  x9, [x9]\n"
    /* Stack inside the snapshot region: the incoming SP points at whatever
     * cothread stack ERTS happened to be on, inside the arena libc_init is
     * about to re-hand-out. */
    "    mov  x10, #" BEAM_RESET_STACK_TOP_STR "\n"
    "    add  sp, x9, x10\n"
    "    bl   beam_reset_restore\n"
    "    mov  sp, x0\n"
    "    b    main\n"
    ".size _reset, . - _reset\n");
