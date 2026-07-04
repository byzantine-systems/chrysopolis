/*
 * Microkit entry point for the beam_server protection domain.
 *
 * beam_server links against the LionsOS POSIX libc (libc.a) and talks to the
 * real sDDF serial and timer driver PDs. init() mirrors the LionsOS reference
 * examples (examples/posix_test): it validates the serial/timer client
 * configs the Microkit tool patched into our ELF sections, initialises the
 * serial TX/RX queue handles, then hands the beam_heap region to libc_init()
 * as the malloc arena and runs the BEAM.
 */
#include <lions/posix/posix.h>

#include <libmicrokitco.h>
#include <lions/fs/config.h>
#include <lions/fs/helpers.h>
#include <lions/fs/protocol.h>
#include <microkit.h>
#include <sddf/network/config.h>
#include <sddf/network/lib_sddf_lwip.h>
#include <sddf/network/queue.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include <sddf/timer/protocol.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Config blobs the sdfgen metaprogram emits and the build objcopies into
 * these ELF sections (.serial_client_config etc.). */
__attribute__((
    __section__(".serial_client_config"))) serial_client_config_t serial_config;
__attribute__((
    __section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".fs_client_config"))) fs_client_config_t fs_config;
__attribute__((
    __section__(".net_client_config"))) net_client_config_t net_config;
__attribute__((__section__(
    ".lib_sddf_lwip_config"))) lib_sddf_lwip_config_t lib_sddf_lwip_config;

/* The libc console path (lib/libc/posix/fd.c) writes through this handle. */
serial_queue_handle_t serial_tx_queue_handle;
serial_queue_handle_t serial_rx_queue_handle;

/* The fs client globals the libc fs path (lib/libc/posix/file.c) and the fs
 * helpers reference. beam_run() points them at the fatfs share/queues from
 * fs_config before libc_init, the libc open/read/stat/lseek path uses them. */
fs_queue_t *fs_command_queue;
fs_queue_t *fs_completion_queue;
char *fs_share;

/* Net client queue handles the linked lwIP stack (lib_sddf_lwip + lib/sock/
 * tcp.c) drives, tcp.c references them extern. socket_config is tcp.c's
 * function-pointer table the prebuilt libc sock.c dereferences once
 * libc_init(&socket_config, ...) enables the socket syscalls. */
net_queue_handle_t net_rx_handle;
net_queue_handle_t net_tx_handle;
extern libc_socket_config_t socket_config;

/* net_enabled: net_config's magic validated (the SDF wired the net client).
 * lwip_up: sddf_lwip_init has run, so notified() may pump the stack (it must
 * never pump an uninitialised stack). dhcp_ready: the DHCP lease has landed. */
static bool net_enabled;
static bool lwip_up;
static bool dhcp_ready;

/* Period of the lwIP service timer (RX poll + lwIP timeouts), matching the
 * LionsOS posix_test client. */
#define NET_TIMEOUT (1 * NS_IN_MS)

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

/* The libc fs path spins here until fatfs records the completion of an fs
 * command. erl_start never returns to the Microkit event loop, so notified()
 * never fires and we cannot block on the fs channel (the reference clients park
 * the cothread and let notified() drain), instead we drain the completion queue
 * directly (fatfs, a higher-priority PD, fills it via seL4 preemption).
 *
 * Crucially this does NOT yield to other cothreads: yielding lets an ERTS
 * helper cothread issue a reentrant fs read mid-command, and the two
 * overlapping reads alias fs_share buffers, corrupting each other's data (a
 * .beam then loads with the wrong module's bytes). Keeping the wait synchronous
 * serialises fs ops exactly as the old memfs reads did. */
static void fs_blocking_wait(microkit_channel ch) {
  (void)ch;
  fs_process_completions(NULL);
}

/* DHCP lease callback: lwIP invokes this when the netif comes up with an IP. */
static void netif_status_callback(char *ip_addr) {
  printf("SOCKET_SMOKE|DHCP: %s\n", ip_addr);
  dhcp_ready = true;
}

/* Bring up the linked lwIP stack over the sDDF net queues and arm the periodic
 * service timer that drives RX/timeout processing from notified(). Mirrors the
 * LionsOS posix_test client's cont(). Runs on the main context after libc_init
 * (so the socket syscalls are registered) and thread_init. */
static void net_init(void) {
  net_queue_init(&net_rx_handle, net_config.rx.free_queue.vaddr,
                 net_config.rx.active_queue.vaddr, net_config.rx.num_buffers);
  net_queue_init(&net_tx_handle, net_config.tx.free_queue.vaddr,
                 net_config.tx.active_queue.vaddr, net_config.tx.num_buffers);
  net_buffers_init(&net_tx_handle, 0);
  sddf_lwip_init(&lib_sddf_lwip_config, &net_config, &timer_config,
                 net_rx_handle, net_tx_handle, NULL, printf,
                 netif_status_callback, NULL, NULL, NULL);
  lwip_up = true;
  sddf_lwip_maybe_notify();
  sddf_timer_set_timeout(timer_config.driver_id, NET_TIMEOUT);
}

/* Deliver Microkit's pending deferred notification immediately, mirroring the
 * libmicrokit handler epilogue (seL4_Send on the stashed signal cap/msg that
 * microkit_deferred_notify set). sddf_lwip_maybe_notify() defers the RX/TX
 * virtualiser signal into Microkit's single-slot pending-signal, which the
 * epilogue only flushes when the PD returns from notified()/init(). Under ERTS
 * erl_start() never returns, so without this flush the deferred signal is
 * stashed but never delivered: net_virt_tx is never woken and the TX queue
 * (DHCP DISCOVER, then TCP segments) never drains. Called from beam_net_pump
 * after sddf_lwip_maybe_notify(). */
static void beam_flush_deferred_notify(void) {
  if (microkit_have_signal) {
    microkit_have_signal = seL4_False;
    seL4_Send(microkit_signal_cap, microkit_signal_msg);
  }
}

/* Pump the linked lwIP stack. Under ERTS, erl_start() never returns to the
 * Microkit event loop, so notified() is dead, instead the poll/sleep syscall
 * stubs ERTS spins on (epoll_pwait, ppoll, pselect6, clock_nanosleep) call this
 * to advance lwIP's RX and flush pending TX. process_timeout is decimated: it
 * costs a timer PPC per call (sys_now) and epoll_pwait is the scheduler's hot
 * idle loop, while TCP/DHCP timers need only coarse (~100 ms) granularity.
 * No-op until net_init has run (lwip_up). */
void beam_net_pump(void) {
  static unsigned pump_count;
  if (!lwip_up) {
    return;
  }
  sddf_lwip_process_rx();
  if ((++pump_count & 63) == 0) {
    sddf_lwip_process_timeout();
  }
  sddf_lwip_maybe_notify();
  beam_flush_deferred_notify();
}

/* Bring-up socket smoke test (no ERTS). Runs on a cothread so notified() can
 * pump lwIP RX/timeouts while we wait for the DHCP lease, then exercises the
 * libc socket path end to end (socket/bind/listen locally, plus a non-blocking
 * connect to the slirp gateway). Prints SOCKET_SMOKE|PASS on success, the
 * socket-smoke nix check gates on it. */
static void socket_smoke(void) {
  printf("SOCKET_SMOKE|START\n");
  while (!dhcp_ready) {
    microkit_cothread_yield();
  }

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    printf("SOCKET_SMOKE|FAIL: socket errno=%d\n", errno);
    return;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(7777);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    printf("SOCKET_SMOKE|FAIL: bind errno=%d\n", errno);
    return;
  }
  if (listen(s, 1) != 0) {
    printf("SOCKET_SMOKE|FAIL: listen errno=%d\n", errno);
    return;
  }
  close(s);

  /* Non-blocking connect() to the slirp gateway returns 0 or EINPROGRESS
   * immediately, proving the connect path is wired without needing a listener.
   */
  int c = socket(AF_INET, SOCK_STREAM, 0);
  if (c < 0) {
    printf("SOCKET_SMOKE|FAIL: socket2 errno=%d\n", errno);
    return;
  }
  fcntl(c, F_SETFL, O_NONBLOCK);
  struct sockaddr_in host;
  memset(&host, 0, sizeof(host));
  host.sin_family = AF_INET;
  host.sin_port = htons(9); /* discard */
  host.sin_addr.s_addr = inet_addr("10.0.2.2");
  int rc = connect(c, (struct sockaddr *)&host, sizeof(host));
  if (rc != 0 && errno != EINPROGRESS) {
    printf("SOCKET_SMOKE|FAIL: connect rc=%d errno=%d\n", rc, errno);
    return;
  }
  close(c);

  printf("SOCKET_SMOKE|PASS\n");
}

static void beam_run(void) {
  /* Wire the libc fs client to the FAT fs_server before libc_init: the real
   * libc_init_file() (called inside libc_init) routes open/read/stat/lseek to
   * the fs protocol, and that path reads these globals + the blocking_wait
   * callback at the first file op. fatfs serves the OTP release ERTS boots
   * from, so this replaces the old in-memory filesystem entirely. */
  fs_command_queue = fs_config.server.command_queue.vaddr;
  fs_completion_queue = fs_config.server.completion_queue.vaddr;
  fs_share = fs_config.server.share.vaddr;
  fs_set_blocking_wait(fs_blocking_wait);

  /* Enable the libc socket layer. Passing a socket config to libc_init makes it
   * call libc_init_sock, which registers socket/bind/connect/listen/accept/
   * getsockopt(SO_ERROR)/... We keep bringup's cothread-yielding ppoll (ERTS
   * boot needs it) rather than sock.c's non-yielding sys_ppoll: sock.c claims
   * __NR_ppoll only when ALL FOUR socket-poll callbacks are set, so we null
   * three in a local copy. We KEEP tcp_socket_err so getsockopt(SO_ERROR)
   * reports real connect completion/errors (ERTS reads it after a nonblocking
   * connect). bringup's socket-aware epoll uses tcp.c's full socket_config
   * table directly for readiness (readable/writable/hup/err). */
  static libc_socket_config_t beam_socket_config;
  beam_socket_config = socket_config;
  beam_socket_config.tcp_socket_readable = NULL;
  beam_socket_config.tcp_socket_writable = NULL;
  beam_socket_config.tcp_socket_hup = NULL;
  libc_init(&beam_socket_config, (void *)beam_heap_start, BEAM_HEAP_SIZE);
  bringup_register_syscalls();
  /* Stand up the cothread runtime ERTS's helper threads spawn onto. Must
   * follow libc_init (uses malloc for the cothread stacks) and precede the
   * FS_CMD_INITIALISE below (fs_command_blocking runs on the cothread runtime).
   */
  thread_init();

  /* Mount the FAT volume. The libc fs path issues per-op commands but never the
   * one-time FS_CMD_INITIALISE that fatfs's f_mount() hangs off, the client
   * must send it, exactly as the LionsOS posix_test / micropython clients do.
   * Without this every open fails (FS_STATUS_ERROR) and ERTS busy-retries
   * forever. */
  {
    fs_cmpl_t cmpl;
    int err = fs_command_blocking(&cmpl, (fs_cmd_t){.type = FS_CMD_INITIALISE});
    if (err != 0 || cmpl.status != FS_STATUS_SUCCESS) {
      printf("FATAL: FAT mount (FS_CMD_INITIALISE) failed: status %d\n",
             (int)cmpl.status);
    } else {
      printf("FAT filesystem mounted via fs_server.\n");
    }
  }

  printf("Chrysopolis: beam_server up on the LionsOS reference stack.\n");

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  printf("monotonic clock via sDDF timer: %lld.%09lld s\n",
         (long long)ts.tv_sec, (long long)ts.tv_nsec);

  /* net_config's magic validates iff the SDF wired the net client (it does when
   * the image is built with the network subsystem). Bring up the linked lwIP
   * stack in BOTH modes so the socket path works: under ERTS the pump is driven
   * by beam_net_pump() from the poll/sleep syscall stubs (erl_start never
   * returns to notified()), in bring-up mode notified() pumps it and we run the
   * socket smoke test. */
  net_enabled = net_config_check_magic(&net_config);
  if (net_enabled) {
    net_init();
  }

  if (erl_start) {
    /* erlexec normally exports these, we bypass it, so erl_prim_loader / init
     * would otherwise abort ("Environment variable BINDIR is not set"). These
     * paths resolve against the FAT filesystem served by fatfs. */
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
    /* Emulator flags come first, everything after `--` is passed to `init`
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
    /* Run the socket smoke test on a cothread. init() then returns to the
     * Microkit event loop, whose notified() pumps lwIP RX/timeouts and wakes
     * the cothread. (lwIP itself was already brought up by net_init above.) */
    if (net_enabled) {
      microkit_cothread_spawn(socket_smoke, NULL);
    } else {
      printf("SOCKET_SMOKE|SKIP: net config magic invalid\n");
    }
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
  /* Pump the linked lwIP stack on the net-RX / service-timer channels before
   * waking any cothread blocked on this channel, so a woken socket cothread
   * sees fresh RX. Gated on lwip_up so we never touch an uninitialised stack.
   * This drives lwIP in bring-up mode, under ERTS notified() never fires
   * (erl_start monopolises the loop) and beam_net_pump() does the pumping. */
  if (lwip_up) {
    if (ch == timer_config.driver_id) {
      sddf_lwip_process_rx();
      sddf_lwip_process_timeout();
      sddf_timer_set_timeout(timer_config.driver_id, NET_TIMEOUT);
    } else if (ch == net_config.rx.id) {
      sddf_lwip_process_rx();
    }
  }
  /* Wake any cothread blocked on this channel (fs/serial/net completions). */
  thread_notified(ch);
  if (lwip_up) {
    sddf_lwip_maybe_notify();
  }
  if (beam_process_external_events) {
    beam_process_external_events(ch);
  }
  microkit_cothread_yield();
}
