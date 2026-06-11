/*
 * Microkit entry point for the beam_server protection domain (Plan B).
 *
 * beam_server links against the LionsOS POSIX libc (libc.a) and talks to the
 * real sDDF serial and timer driver PDs. init() mirrors the LionsOS reference
 * examples (examples/posix_test): it validates the serial/timer client
 * configs the Microkit tool patched into our ELF sections, initialises the
 * serial TX/RX queue handles, then hands the beam_heap region to libc_init()
 * as the malloc arena and runs the BEAM.
 *
 * erl_start comes out of liberts.a (Phase 4+); it is weak so the same image
 * boots in bring-up mode (console + clock + heap, no ERTS) before the static
 * ERTS archive joins the link.
 */
#include <lions/posix/posix.h>

#include <libmicrokitco.h>
#include <lions/fs/config.h>
#include <lions/fs/helpers.h>
#include <lions/fs/protocol.h>
#include <microkit.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/config.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Config blobs the sdfgen metaprogram emits and the build objcopies into
 * these ELF sections (.serial_client_config etc.). */
__attribute__((
    __section__(".serial_client_config"))) serial_client_config_t serial_config;
__attribute__((
    __section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".fs_client_config"))) fs_client_config_t fs_config;

/* The libc console path (lib/libc/posix/fd.c) writes through this handle. */
serial_queue_handle_t serial_tx_queue_handle;
serial_queue_handle_t serial_rx_queue_handle;

/* The fs client globals the libc fs path references. Defined (so libc.a
 * links) but dormant in bring-up: no fs_server PD is wired yet, so the
 * config section stays zero and the queues stay NULL. File I/O is a Step 2
 * concern (fat fs_server + blk + libmicrokitco). */
fs_queue_t *fs_command_queue;
fs_queue_t *fs_completion_queue;
char *fs_share;

/* beam_heap region base, patched by the Microkit tool at synthesis time
 * (setvar_vaddr). Handed to libc_init() as the malloc arena, which
 * replaces the old hand-rolled mmap.c bump allocator. */
uintptr_t beam_heap_start;
#define BEAM_HEAP_SIZE 0x20000000

extern void erl_start(int argc, char **argv) __attribute__((weak));
extern void beam_process_external_events(microkit_channel ch)
    __attribute__((weak));

extern void bringup_register_syscalls(void);
extern void thread_init(void);
extern void thread_notified(microkit_channel ch);
extern void memfs_init(void);

/* The libc fs path blocks here waiting for an fatfs completion; a cothread
 * yields to the scheduler and is woken by the fs channel notification. */
static void fs_blocking_wait(microkit_channel ch) {
  microkit_cothread_wait_on_channel(ch);
}

static void beam_run(void) {
  libc_init(NULL, (void *)beam_heap_start, BEAM_HEAP_SIZE);
  memfs_init(); /* Initialize in-memory filesystem before syscall registration
                 */
  bringup_register_syscalls();
  /* Stand up the cothread runtime ERTS's helper threads spawn onto. Must
   * follow libc_init (uses malloc for the cothread stacks). */
  thread_init();

  /* Wire the libc fs client: DISABLED for Option C testing
   * Testing ERTS boot without filesystem */
  // fs_set_blocking_wait(fs_blocking_wait);
  // fs_command_queue = fs_config.server.command_queue.vaddr;
  // fs_completion_queue = fs_config.server.completion_queue.vaddr;
  // fs_share = fs_config.server.share.vaddr;

  printf("Chrysopolis: beam_server up on the LionsOS reference stack.\n");

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  printf("monotonic clock via sDDF timer: %lld.%09lld s\n",
         (long long)ts.tv_sec, (long long)ts.tv_nsec);

  if (erl_start) {
    /* erlexec normally exports these; we bypass it, so erl_prim_loader / init
     * would otherwise abort ("Environment variable BINDIR is not set"). The
     * paths are nominal until the fs_server is wired (Step 2d). */
    setenv("BINDIR", "/bin", 1);
    setenv("ROOTDIR", "/", 1);
    setenv("EMU", "beam", 1);
    setenv("PROGNAME", "beam", 1);

    /* -MIscs 256: cap the ERTS literal super carrier at 256 MiB so it fits
     * inside the 512 MiB beam_heap region. ERTS reserves a 1024 MiB
     * contiguous literal carrier by default (for 64-bit literal-term
     * addressing), which the bump allocator over beam_heap cannot satisfy.
     * 256 MiB is ample for boot + OTP + the Gleam payload's literals.
     * NOTE: the emulator parses allocator flags in their `-M...` form;
     * erlexec normally translates the user-facing `+M...`. We bypass erlexec
     * (calling erl_start directly), so we pass the `-M` form. */
    /* Emulator flags come first; everything after `--` is passed to `init`
     * (erl_prim_loader reads -root/-boot from there to find the boot script).
     * Without the `--`, the emulator treats -root as an unknown flag and
     * prints usage. */
    /* -Bd: disable the Ctrl+C break handler (its thread otherwise select()s
     * on the console forever, which we cannot service).
     * After `--`, the init args erlexec normally supplies: -root, -bindir
     * (erl_prim_loader requires the command-line -bindir, distinct from the
     * BINDIR env var), -boot (the boot script), embedded mode. */
    static char *argv[] = {
        "beam",
        "-MIscs",
        "256",
        "-Bd",
        "--",
        "-root",
        "/",
        "-bindir",
        "/bin",
        "-boot",
        "/releases/28/start",
        "-mode",
        "embedded",
        NULL,
    };
    printf("Handing off to ERTS core loop...\n");
    erl_start(13, argv);
  } else {
    printf("liberts.a not linked: bring-up mode (console + clock + heap).\n");
  }
}

void init(void) {
  assert(serial_config_check_magic(&serial_config));
  assert(timer_config_check_magic(&timer_config));

  if (serial_config.rx.queue.vaddr != NULL) {
    serial_queue_init(&serial_rx_queue_handle, serial_config.rx.queue.vaddr,
                      serial_config.rx.data.size, serial_config.rx.data.vaddr);
  }
  serial_queue_init(&serial_tx_queue_handle, serial_config.tx.queue.vaddr,
                    serial_config.tx.data.size, serial_config.tx.data.vaddr);

  beam_run();
}

void notified(microkit_channel ch) {
  /* Wake any cothread blocked on this channel (fs/serial completions). */
  thread_notified(ch);
  if (beam_process_external_events) {
    beam_process_external_events(ch);
  }
  microkit_cothread_yield();
}
