/*
 * Bring-up syscall shims for the beam_server PD.
 *
 * The LionsOS libc only registers the syscalls its own components need, ERTS
 * reaches for a number more during boot (epoll/pselect6/ppoll for its poll set,
 * pipe2/timerfd/socketpair, a cothread-aware futex, sched_getaffinity for the
 * CPU count, uname, clock_nanosleep, signal and scheduler no-ops). libc leaves
 * those slots NULL -> sel4_vsyscall returns -ENOSYS and ERTS spins, so
 * bringup_register_syscalls() installs benign stubs (called after libc_init,
 * while the slots are still free, so no double-register assert).
 *
 * File I/O (open/read/write/stat/lseek) is NOT handled here: the real libc fs
 * path (lib/libc/posix/file.c, wired in main.c) routes it to the FAT fs_server.
 * TCP/IP is NOT handled here either: gen_tcp drives the real libc socket layer
 * (sock.c -> tcp.c -> lwIP), and this file's epoll/ppoll stubs report socket
 * readiness from tcp.c and pump lwIP (beam_net_pump). The pipe2/timerfd/
 * socketpair stubs that remain are ERTS-internal plumbing (poll self-pipe,
 * timerfd-built emulator, spawn_init's AF_UNIX pair), deliberate shims, not a
 * substitute for networking.
 */
#include <lions/posix/fd.h>
#include <lions/posix/posix.h>

#include "rng.h"

#include <microkit.h>
#include <sel4/sel4.h>

#include <libmicrokitco.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Serial RX queue handle for console input, defined in main.c. The TX handle
 * is used to echo keystrokes back to the console (see console_read). */
extern serial_queue_handle_t serial_rx_queue_handle;
extern serial_queue_handle_t serial_tx_queue_handle;
extern serial_client_config_t serial_config;
/* Timer client config (driver_id channel), defined in main.c. Used by
 * clock_nanosleep to read the monotonic clock for a real timed sleep. */
extern timer_client_config_t timer_config;

/* tcp.c's full socket_config (readable/writable/hup/err intact), distinct from
 * the poll-callback-nulled copy main.c hands libc_init. The socket-aware epoll
 * below queries readiness through it. */
extern libc_socket_config_t socket_config;
/* Advances the linked lwIP stack (RX/TX). Defined in main.c, called from the
 * poll/sleep stubs so lwIP progresses while ERTS spins (notified() is dead once
 * erl_start runs). */
extern void beam_net_pump(void);

#define SERIAL_RX_CH 1

/* Discard sink / EOF source for the synthetic fds below (pipe write end,
 * timerfd, socketpair): writes are discarded, reads return EOF. */
static ssize_t devnull_write(const void *data, size_t count, int fd) {
  (void)data;
  (void)fd;
  return (ssize_t)count;
}

static ssize_t devnull_read(void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  (void)fd;
  return 0;
}

static int devnull_close(int fd) { return posix_fd_deallocate(fd); }

/* An always-empty read end (pipes, timerfd): no data will ever arrive. A
 * non-blocking fd reports EAGAIN (ERTS's poll wakeup pipe drains this way), a
 * blocking fd parks the calling cothread forever (ERTS's signal-dispatcher
 * thread blocks reading the signal pipe, no OS signals exist here, so it
 * simply never returns, which is correct). */
static ssize_t empty_read(void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  fd_entry_t *e = posix_fd_entry(fd);
  if (e != NULL && (e->flags & O_NONBLOCK)) {
    return -EAGAIN;
  }
  for (;;) {
    microkit_cothread_yield();
  }
}

/* Echo a keystroke back to the console. QEMU's -serial mon:stdio raw-mode host
 * terminal does not echo, and ERTS runs in dumb-terminal mode (its prim_tty
 * line editor can't init through our ioctl stubs), so it neither echoes nor
 * edits — without this the user types blind. Cooked-mode echo: CR is shown as
 * CRLF (Enter over serial arrives as '\r'), DEL/BS erase the last glyph. */
static void console_echo(char c) {
  switch (c) {
  case '\r':
  case '\n':
    serial_enqueue_batch(&serial_tx_queue_handle, 2, "\r\n");
    break;
  case 0x7f: /* DEL */
  case 0x08: /* BS  */
    serial_enqueue_batch(&serial_tx_queue_handle, 3, "\b \b");
    break;
  default:
    serial_enqueue(&serial_tx_queue_handle, c);
    break;
  }
  microkit_notify(serial_config.tx.id);
}

/* Console read: dequeue characters from the serial RX queue into the user
 * buffer. For blocking reads (stdin default), yields the cothread when the
 * queue is empty to let the RX virtualiser refill it. For non-blocking reads
 * (O_NONBLOCK set via fcntl), returns -EAGAIN when empty. Each dequeued byte is
 * echoed back to the console (console_echo). */
static ssize_t console_read(void *data, size_t count, int fd) {
  char *buf = (char *)data;
  size_t n = 0;

  fd_entry_t *e = posix_fd_entry(fd);
  bool nonblock = (e != NULL && (e->flags & O_NONBLOCK));

  while (n < count) {
    char c;
    if (serial_dequeue(&serial_rx_queue_handle, &c) == 0) {
      buf[n++] = c;
      console_echo(c);
    } else {
      if (n > 0)
        break;
      if (nonblock)
        return -EAGAIN;
      microkit_cothread_wait_on_channel(SERIAL_RX_CH);
    }
  }
  return (ssize_t)n;
}

/* Allocate an fd backed by the discard/empty callbacks. */
static int bringup_alloc_fd(fd_read_func read, fd_write_func write, int flags) {
  int fd = posix_fd_allocate();
  if (fd < 0) {
    return -EMFILE;
  }
  fd_entry_t *e = posix_fd_entry(fd);
  if (e == NULL) {
    return -EMFILE;
  }
  e->read = read;
  e->write = write;
  e->close = devnull_close;
  e->flags = flags;
  return fd;
}

/* pipe2: ERTS-only shim. ERTS builds a self-pipe to wake its own poll set from
 * a signal/aux thread, it is internal plumbing, not a socket. The write end
 * discards and the read end is always empty, our epoll stub wakes on real
 * readiness (serial RX, socket state) instead of via this pipe. */
static long bringup_pipe2(va_list ap) {
  int *pipefd = va_arg(ap, int *);
  int flags = va_arg(ap, int);
  if (pipefd == NULL) {
    return -EFAULT;
  }
  /* Preserve O_NONBLOCK so the read end blocks or returns EAGAIN correctly. */
  int rfd = bringup_alloc_fd(empty_read, NULL, O_RDONLY | (flags & O_NONBLOCK));
  if (rfd < 0) {
    return rfd;
  }
  int wfd = bringup_alloc_fd(NULL, devnull_write, O_WRONLY);
  if (wfd < 0) {
    return wfd;
  }
  pipefd[0] = rfd;
  pipefd[1] = wfd;
  return 0;
}

/* timerfd: ERTS-only shim. The emulator was cross-built with timerfd support
 * (its time-correction machinery), unrelated to sockets. Hand back a
 * non-blocking fd that never reports expirations, the sDDF timer drives the
 * real monotonic clock (clock_nanosleep/clock_gettime). */
static long bringup_timerfd_create(va_list ap) {
  (void)ap;
  return bringup_alloc_fd(empty_read, devnull_write, O_RDWR | O_NONBLOCK);
}

/* socketpair: the ERTS-only shim. ERTS's spawn_init opens a unix-domain
 * (AF_UNIX) socketpair to talk to its port forker (erl_child_setup). This is
 * unrelated to the real AF_INET socket path (sock.c + lwIP, enabled via
 * libc_init in main.c): sock.c does not implement socketpair, and we cannot
 * spawn OS processes, but spawn_init must succeed for ERTS to boot, so we hand
 * back two valid non-blocking fds. Port operations over them simply never
 * complete. */
static long bringup_socketpair(va_list ap) {
  (void)va_arg(ap, int); /* domain */
  (void)va_arg(ap, int); /* type */
  (void)va_arg(ap, int); /* protocol */
  int *sv = va_arg(ap, int *);
  if (sv == NULL) {
    return -EFAULT;
  }
  int a = bringup_alloc_fd(empty_read, devnull_write, O_RDWR | O_NONBLOCK);
  if (a < 0) {
    return a;
  }
  int b = bringup_alloc_fd(empty_read, devnull_write, O_RDWR | O_NONBLOCK);
  if (b < 0) {
    return b;
  }
  sv[0] = a;
  sv[1] = b;
  return 0;
}

static long bringup_zero(va_list ap) {
  (void)ap;
  return 0;
}

/* Cothread-aware futex. ERTS's low-level thread-event primitive (ethr_event)
 * uses futex directly and treats ENOSYS as fatal. Under cooperative
 * cothreads, FUTEX_WAIT yields (letting the waker cothread run) until the
 * word changes or a bounded number of rounds elapse (spurious return, ERTS
 * re-checks its event flag). FUTEX_WAKE is a no-op: waiters observe the word
 * when they next run. */
#define FUTEX_WAIT_OP 0
#define FUTEX_WAKE_OP 1
#define FUTEX_WAIT_BITSET_OP 9
#define FUTEX_WAKE_BITSET_OP 10

static long bringup_futex(va_list ap) {
  int *uaddr = va_arg(ap, int *);
  int op = va_arg(ap, int);
  int val = va_arg(ap, int);
  int cmd = op & 0x7f; /* strip FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME */

  if (cmd == FUTEX_WAKE_OP || cmd == FUTEX_WAKE_BITSET_OP) {
    return 0;
  }
  /* FUTEX_WAIT / FUTEX_WAIT_BITSET */
  if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
    return -EAGAIN;
  }
  /* DIAG wait-latency tracing (uncomment sparingly: the print itself floods
   * the serial path and distorts the timing being measured):
   * uint64_t f0 = sddf_timer_time_now(timer_config.driver_id); */
  for (int i = 0; i < 4096; i++) {
    microkit_cothread_yield();
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
      // uint64_t f1 = sddf_timer_time_now(timer_config.driver_id);
      // if (f1 - f0 > 100000000ull) {
      //   printf("DIAG|%lu|futex woke after %lums\n",
      //          (unsigned long)(f1 / 1000000ull),
      //          (unsigned long)((f1 - f0) / 1000000ull));
      // }
      return 0;
    }
  }
  // uint64_t f2 = sddf_timer_time_now(timer_config.driver_id);
  // if (f2 - f0 > 100000000ull) {
  //   printf("DIAG|%lu|futex spurious after %lums\n",
  //          (unsigned long)(f2 / 1000000ull),
  //          (unsigned long)((f2 - f0) / 1000000ull));
  // }
  return 0; /* spurious, the caller re-checks */
}

/* musl's _Exit() calls exit_group then loops on exit, neither is implemented
 * by the libc, so an ERTS abort spins forever flooding the console. A Microkit
 * PD cannot truly exit, so we announce the code once and park the PD quietly
 * (the monitor reports the fault context if needed). */
static long bringup_exit(va_list ap) {
  (void)va_arg(ap, int); /* status */
  microkit_dbg_puts("beam_server: ERTS requested exit; parking PD.\n");
  for (;;) {
    seL4_Yield();
  }
}

/* ERTS reads the CPU affinity mask to size its scheduler pool, report a
 * single online CPU (matches the +S 1:1 single-threaded boot). */
static long bringup_sched_getaffinity(va_list ap) {
  (void)va_arg(ap, long); /* pid */
  size_t cpusetsize = va_arg(ap, size_t);
  unsigned long *mask = va_arg(ap, unsigned long *);
  if (mask != NULL && cpusetsize >= sizeof(unsigned long)) {
    memset(mask, 0, cpusetsize);
    mask[0] = 1;
  }
  return (long)sizeof(unsigned long);
}

/* ERTS queries the monotonic clock resolution, report a 1ns granularity. */
static long bringup_clock_getres(va_list ap) {
  (void)va_arg(ap, long); /* clockid */
  struct timespec *res = va_arg(ap, struct timespec *);
  if (res != NULL) {
    res->tv_sec = 0;
    res->tv_nsec = 1;
  }
  return 0;
}

/* clock_nanosleep: a real timed sleep (musl's nanosleep/usleep/sleep all route
 * here via SYS_clock_nanosleep). The old stub returned immediately, making
 * those calls no-ops. We busy-poll the sDDF monotonic clock and yield to the
 * cothread scheduler until the deadline -- the SAME pattern process.c's
 * pthread_cond_timedwait uses. We deliberately do NOT block on a notification
 * (microkit_cothread_wait_on_channel): erl_start runs on the root cothread and
 * never returns, so notified()/recv_ntfn never fire and such a wait would hang
 * forever (the whole PD works by busy-polling shared memory). */
static long bringup_clock_nanosleep(va_list ap) {
  (void)va_arg(ap, long); /* clockid */
  int flags = va_arg(ap, int);
  const struct timespec *req = va_arg(ap, const struct timespec *);
  struct timespec *rem = va_arg(ap, struct timespec *);
  if (req == NULL) {
    return -EFAULT;
  }
  if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
    return -EINVAL;
  }
  unsigned ch = timer_config.driver_id;
  uint64_t reqns =
      (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
  uint64_t now0 = sddf_timer_time_now(ch);
  uint64_t target = (flags & TIMER_ABSTIME) ? reqns : now0 + reqns;
  // if (target > now0 && target - now0 > 100000000ull) { /* DIAG */
  //   printf("DIAG|%lu|nanosleep %lums\n", (unsigned long)(now0 / 1000000ull),
  //          (unsigned long)((target - now0) / 1000000ull));
  // }
  while (sddf_timer_time_now(ch) < target) {
    /* Keep lwIP advancing during the sleep so TCP timers/RX don't stall while
     * a scheduler cothread is parked here. */
    beam_net_pump();
    microkit_cothread_yield();
  }
  if (rem != NULL && !(flags & TIMER_ABSTIME)) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

/* ERTS queries RLIMIT_NOFILE to size its fd tables, report a fixed limit. */
static long bringup_getrlimit(va_list ap) {
  (void)va_arg(ap, int); /* resource */
  struct rlimit *rl = va_arg(ap, struct rlimit *);
  if (rl != NULL) {
    rl->rlim_cur = 1024;
    rl->rlim_max = 1024;
  }
  return 0;
}

static long bringup_prlimit(va_list ap) {
  (void)va_arg(ap, int);    /* pid */
  (void)va_arg(ap, int);    /* resource */
  (void)va_arg(ap, void *); /* new_limit */
  struct rlimit *old = va_arg(ap, struct rlimit *);
  if (old != NULL) {
    old->rlim_cur = 1024;
    old->rlim_max = 1024;
  }
  return 0;
}

/* Real epoll: ERTS's kernel-poll I/O subsystem monitors stdin (fd 0) via
 * epoll_ctl(ADD) + epoll_pwait. We track registered fds and report fd 0
 * readable when the serial RX queue has data, so console input reaches the
 * shell. The epoll_event.data the caller registers must be returned verbatim
 * in epoll_pwait, since ERTS uses it to identify the I/O source. */
#define EPOLL_MAX_FDS 64
static struct {
  int fd;
  struct epoll_event ev;
  bool active;
  bool armed;   /* EPOLLONESHOT: disarmed after one report, re-armed by MOD */
  bool is_sock; /* fd is a socket (readiness comes from tcp.c, not serial) */
  int sock_handle; /* tcp.c socket index, valid iff is_sock */
} epoll_table[EPOLL_MAX_FDS];

/* True iff fd is a socket fd (its fd_entry->fstat reports S_IFSOCK). Safe on
 * any fd, only after this returns true is socket_index_of_fd (which asserts)
 * valid. */
static bool fd_is_socket(int fd) {
  fd_entry_t *e = posix_fd_entry(fd);
  struct stat st;
  return e != NULL && e->fstat != NULL && e->fstat(fd, &st) == 0 &&
         S_ISSOCK(st.st_mode);
}

static long bringup_epoll_create(va_list ap) {
  (void)ap;
  int fd = posix_fd_allocate();
  if (fd < 0) {
    return -EMFILE;
  }
  fd_entry_t *e = posix_fd_entry(fd);
  if (e == NULL) {
    return -EMFILE;
  }
  e->read = devnull_read;
  e->write = devnull_write;
  e->close = devnull_close;
  e->flags = O_RDWR;
  return fd;
}

/* epoll_ctl: register/update/remove fds in our table. */
static long bringup_epoll_ctl(va_list ap) {
  int epfd = va_arg(ap, int);
  int op = va_arg(ap, int);
  int fd = va_arg(ap, int);
  struct epoll_event *event = va_arg(ap, struct epoll_event *);
  (void)epfd;

  if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
    if (event == NULL) {
      return -EFAULT;
    }
    int slot = -1;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
      if (epoll_table[i].active && epoll_table[i].fd == fd) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        if (!epoll_table[i].active) {
          slot = i;
          break;
        }
      }
    }
    if (slot < 0) {
      return -ENOSPC;
    }
    epoll_table[slot].fd = fd;
    epoll_table[slot].ev = *event;
    epoll_table[slot].active = true;
    epoll_table[slot].armed = true; /* (re-)arm on ADD or MOD */
    epoll_table[slot].is_sock = fd_is_socket(fd);
    epoll_table[slot].sock_handle =
        epoll_table[slot].is_sock ? socket_index_of_fd(fd) : -1;
    // printf("DIAG|%lu|epoll_ctl %s fd=%d ev=%x sock=%d\n",
    //        (unsigned long)(sddf_timer_time_now(timer_config.driver_id) /
    //                        1000000ull),
    //        op == EPOLL_CTL_ADD ? "ADD" : "MOD", fd, event->events,
    //        epoll_table[slot].is_sock ? epoll_table[slot].sock_handle : -1);
  } else if (op == EPOLL_CTL_DEL) {
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
      if (epoll_table[i].active && epoll_table[i].fd == fd) {
        epoll_table[i].active = false;
        break;
      }
    }
  }
  return 0;
}

/* epoll_pwait: scan registered fds for readiness. fd 0 (stdin) is readable when
 * the serial RX queue has data, socket fds report readiness from tcp.c. This is
 * the scheduler's hot idle loop, so it also pumps lwIP (beam_net_pump), under
 * ERTS that is the only thing advancing the network. Return matching events
 * with the caller's registered data, yield when nothing is ready.
 *
 * When timeout > 0 and no events are ready, this stub sleeps for a fraction of
 * the requested timeout (capped at 10 ms per round) to give QEMU's main loop a
 * chance to poll slirp's hostfwd listen fds. Without this sleep, the vCPU
 * busy-spins nonstop, QEMU's main loop is starved, and host→guest TCP SYNs
 * (injected by slirp's hostfwd accept handler, which runs in QEMU's main loop)
 * never reach the virtio-net device. Guest→host traffic (DHCP, outgoing
 * connect) is unaffected because it originates in the vCPU thread and the
 * response is delivered synchronously via virtio MMIO. */
static long bringup_epoll_pwait(va_list ap) {
  int epfd = va_arg(ap, int);
  struct epoll_event *events = va_arg(ap, struct epoll_event *);
  int maxevents = va_arg(ap, int);
  int timeout = va_arg(ap, int);
  (void)epfd;

  if (events == NULL || maxevents <= 0) {
    return -EINVAL;
  }

  beam_net_pump();

  int n = 0;
  for (int i = 0; i < EPOLL_MAX_FDS && n < maxevents; i++) {
    if (!epoll_table[i].active || !epoll_table[i].armed) {
      continue;
    }
    uint32_t revents = 0;
    if (epoll_table[i].is_sock) {
      int h = epoll_table[i].sock_handle;
      if (socket_config.tcp_socket_readable(h)) {
        revents |= EPOLLIN;
      }
      if (socket_config.tcp_socket_writable(h)) {
        revents |= EPOLLOUT;
      }
      if (socket_config.tcp_socket_hup(h)) {
        revents |= EPOLLHUP;
      }
      if (socket_config.tcp_socket_err(h) != 0) {
        revents |= EPOLLERR;
      }
    } else if (epoll_table[i].fd == 0) {
      if (serial_queue_length_consumer(&serial_rx_queue_handle) > 0) {
        revents |= EPOLLIN;
      }
    }
    revents &= (epoll_table[i].ev.events | EPOLLERR | EPOLLHUP);
    if (revents) {
      // if (epoll_table[i].is_sock) {
      //   printf("DIAG|%lu|epoll deliver sock=%d ev=%x to=%d\n",
      //          (unsigned long)(sddf_timer_time_now(timer_config.driver_id) /
      //                          1000000ull),
      //          epoll_table[i].sock_handle, revents, timeout);
      // }
      events[n].events = revents;
      events[n].data = epoll_table[i].ev.data;
      n++;
      /* EPOLLONESHOT: disable this fd until ERTS re-arms via EPOLL_CTL_MOD.
       * This prevents the scheduler from spinning on the same event and
       * lets the dirty-IO cothread run read(0) -> console_read. */
      if (epoll_table[i].ev.events & EPOLLONESHOT) {
        epoll_table[i].armed = false;
      }
    }
  }

  if (n == 0) {
    /* No events ready: sleep a fraction of the requested timeout (or a
     * minimum 100 ms if timeout <= 0) to give QEMU's main loop time to poll
     * slirp's hostfwd listen fd. ERTS passes timeout=-1 when idle, on a
     * real OS this blocks the thread and the CPU goes idle. Without this
     * sleep the vCPU busy-spins nonstop, QEMU's main loop is starved, and
     * host->guest TCP SYNs (injected by slirp's hostfwd accept handler,
     * which runs in QEMU's main loop) never reach the virtio-net device.
     * The sleep busy-polls the sDDF monotonic clock and pumps lwIP, so the
     * network still progresses even while the scheduler "blocks". */
    struct timespec ts;
    long sleep_ms = timeout > 0 ? (timeout < 20 ? timeout : 20) : 20;
    ts.tv_sec = 0;
    ts.tv_nsec = sleep_ms * 1000000L;
    unsigned ch = timer_config.driver_id;
    uint64_t target = sddf_timer_time_now(ch) + (uint64_t)ts.tv_nsec;
    while (sddf_timer_time_now(ch) < target) {
      beam_net_pump();
      microkit_cothread_yield();
    }
  }

  /* Always yield so the dirty-IO scheduler cothread (which performs the
   * deferred read via the prim_tty NIF) gets a chance to run. */
  microkit_cothread_yield();
  return n;
}

/* ppoll: ERTS's scheduler polls for I/O when idle. Returning immediately would
 * busy-spin the scheduler cothread and starve the helper cothreads (aux, poll,
 * dirty-I/O), stalling boot. Yield first so the cothread scheduler runs the
 * others, then report no ready events. Also pumps lwIP (a backstop for the
 * epoll_pwait pump: ERTS also polls via ppoll). */
static long bringup_poll_yield(va_list ap) {
  struct pollfd *fds = va_arg(ap, struct pollfd *);
  nfds_t nfds = va_arg(ap, nfds_t);

  beam_net_pump();

  /* Report real readiness: socket fds from tcp.c's socket_config, stdin from
   * the serial RX queue. Returning 0 for a ready socket would leave ERTS's
   * fallback pollset blind (and 0 on an infinite timeout is a spec violation
   * ERTS responds to by re-calling in a hot loop). */
  int ready = 0;
  for (nfds_t i = 0; i < nfds && fds != NULL; i++) {
    fds[i].revents = 0;
    int fd = fds[i].fd;
    if (fd < 0) {
      continue;
    }
    if (fd == 0) {
      if ((fds[i].events & POLLIN) &&
          serial_queue_length_consumer(&serial_rx_queue_handle) > 0) {
        fds[i].revents |= POLLIN;
      }
    } else if (fd_is_socket(fd)) {
      int h = socket_index_of_fd(fd);
      if ((fds[i].events & POLLIN) && socket_config.tcp_socket_readable(h)) {
        fds[i].revents |= POLLIN;
      }
      if ((fds[i].events & POLLOUT) && socket_config.tcp_socket_writable(h)) {
        fds[i].revents |= POLLOUT;
      }
      if (socket_config.tcp_socket_hup(h)) {
        fds[i].revents |= POLLHUP;
      }
      if (socket_config.tcp_socket_err(h) != 0) {
        fds[i].revents |= POLLERR;
      }
    }
    if (fds[i].revents) {
      ready++;
    }
  }

  // if (ready) {
  //   printf("DIAG|%lu|ppoll ready=%d fd0=%d rev0=%x\n",
  //          (unsigned long)(sddf_timer_time_now(timer_config.driver_id) /
  //                          1000000ull),
  //          ready, fds ? fds[0].fd : -1, fds ? fds[0].revents : 0);
  // }
  microkit_cothread_yield();
  return ready;
}

/* pselect6: select()/pselect6 syscall, parse fd_sets and report which fds
 * are ready. ERTS doesn't use this for stdin (it uses epoll), but other tools
 * (e.g. the shell's select() timeout sleeps) may call it. */
static long bringup_pselect6(va_list ap) {
  int nfds = va_arg(ap, int);
  fd_set *readfds = va_arg(ap, fd_set *);
  fd_set *writefds = va_arg(ap, fd_set *);
  fd_set *exceptfds = va_arg(ap, fd_set *);
  const struct timespec *timeout = va_arg(ap, const struct timespec *);
  const sigset_t *sigmask = va_arg(ap, const sigset_t *);

  (void)timeout;
  (void)sigmask;

  if (nfds < 0 || nfds > FD_SETSIZE) {
    return -EINVAL;
  }

  int ready = 0;

  /* Check stdin (fd 0) if it's in the readfds set */
  if (readfds != NULL && FD_ISSET(0, readfds)) {
    if (serial_queue_length_consumer(&serial_rx_queue_handle) > 0) {
      /* Keep fd 0 set - data is ready */
      ready++;
    } else {
      /* Clear fd 0 - no data ready */
      FD_CLR(0, readfds);
    }
  }

  /* Clear write and except sets (not implemented) */
  if (writefds != NULL) {
    FD_ZERO(writefds);
  }
  if (exceptfds != NULL) {
    FD_ZERO(exceptfds);
  }

  /* If nothing ready, pump lwIP and yield to other cothreads before returning
   */
  if (ready == 0) {
    beam_net_pump();
    microkit_cothread_yield();
  }

  return ready;
}

/* getcwd: erl_prim_loader needs a current directory. Report root. The syscall
 * fills the buffer and returns the length including the NUL terminator. */
static long bringup_getcwd(va_list ap) {
  char *buf = va_arg(ap, char *);
  size_t size = va_arg(ap, size_t);
  if (buf == NULL) {
    return -EFAULT;
  }
  if (size < 2) {
    return -ERANGE;
  }
  buf[0] = '/';
  buf[1] = '\0';
  return 2;
}

/* getuid/geteuid/getgid/getegid: Identity syscalls. Return 0 (root). */
static long bringup_getid_zero(va_list ap) {
  (void)ap;
  return 0;
}

static long bringup_uname(va_list ap) {
  struct utsname *u = va_arg(ap, struct utsname *);
  if (u != NULL) {
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "chrysopolis");
    strcpy(u->release, "6.0.0-sel4");
    strcpy(u->version, "Chrysopolis LionsOS");
    strcpy(u->machine, "aarch64");
  }
  return 0;
}

/* ---- Real entropy (issue 0.3.0-rng): getrandom / clock_gettime / openat.
 *
 * libc_init (posix.c) and libc_init_file (file.c) claim these three slots
 * before bringup_register_syscalls runs, and libc_define_syscall asserts the
 * slot is NULL, so we can't re-register them. libc_redefine_syscall (our tiny
 * LionsOS libc patch, applied in flake.nix's lionsosSrc) instead REPLACES a
 * claimed slot and returns the old handler, so these shims can chain to the
 * upstream behaviour. See rng.c for the DRBG. ---- */

/* Old handlers returned by libc_redefine_syscall, for chaining. */
static muslcsys_syscall_t old_clock_gettime;
static muslcsys_syscall_t old_openat;

/* 2026-01-01T00:00:00Z. A sane base epoch so CLOCK_REALTIME is roughly correct
 * (future TLS cert validity, log timestamps) instead of starting near 0. The
 * real fix (RTC/NTP) is a separate future issue; here we only need it non-zero
 * and per-boot-varying. */
#define RNG_REALTIME_BASE_EPOCH 1767225600ull

/* getrandom(2): fill the caller's buffer from the DRBG. Replaces LionsOS's
 * unseeded rand() loop (posix.c sys_getrandom, "deliberately insecure"). */
static long bringup_getrandom(va_list ap) {
  void *buf = va_arg(ap, void *);
  size_t buflen = va_arg(ap, size_t);
  (void)va_arg(ap, unsigned); /* flags: GRND_NONBLOCK/GRND_RANDOM — we never
                               * block and never run dry, so both are moot */
  if (buf == NULL) {
    return -EFAULT;
  }
  rng_fill((uint8_t *)buf, buflen);
  return (long)buflen;
}

/* clock_gettime: chain to the upstream sDDF-timer handler, then for
 * CLOCK_REALTIME *only* add the base epoch plus the per-boot entropy-derived
 * offset. This is what makes erlang:make_ref()/rand differ across boots (ERTS
 * seeds them from gettimeofday, which LionsOS aliases to the monotonic timer
 * that starts at ~0 every boot). CLOCK_MONOTONIC is passed through untouched.
 *
 * We va_copy the arg list so the old handler consumes a fresh cursor while we
 * still read clk_id/tp ourselves to decide whether to apply the offset. */
static long bringup_clock_gettime(va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  long ret = old_clock_gettime ? old_clock_gettime(copy) : -ENOSYS;
  va_end(copy);
  if (ret != 0) {
    return ret;
  }
  clockid_t clk_id = va_arg(ap, clockid_t);
  struct timespec *tp = va_arg(ap, struct timespec *);
  if (clk_id == CLOCK_REALTIME && tp != NULL) {
    tp->tv_sec += (time_t)(RNG_REALTIME_BASE_EPOCH + rng_realtime_offset_sec);
  }
  return ret;
}

/* read callback for the virtual /dev/urandom, /dev/random fds. Never EOFs. */
static ssize_t urandom_read(void *data, size_t count, int fd) {
  (void)fd;
  rng_fill((uint8_t *)data, count);
  return (ssize_t)count;
}

/* True for the RNG device paths ERTS / file:read may open. Matches both the
 * absolute form and the cwd-relative form (cwd is "/", see bringup_getcwd). */
static bool is_rng_device_path(const char *path) {
  return strcmp(path, "/dev/urandom") == 0 ||
         strcmp(path, "/dev/random") == 0 || strcmp(path, "dev/urandom") == 0 ||
         strcmp(path, "dev/random") == 0;
}

/* openat: intercept /dev/urandom and /dev/random, handing back an fd whose read
 * callback pulls from the DRBG. Everything else chains to the real libc fs path
 * (file.c sys_openat -> the FAT fs_server). The empty ::/dev/urandom FAT file
 * is dropped from the disk image (flake.nix), so without this intercept those
 * reads would return EOF. */
static long bringup_openat(va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  (void)va_arg(ap, int);                       /* dirfd */
  const char *path = va_arg(ap, const char *); /* pathname */
  if (path != NULL && is_rng_device_path(path)) {
    va_end(copy);
    return bringup_alloc_fd(urandom_read, NULL, O_RDONLY);
  }
  long ret = old_openat ? old_openat(copy) : -ENOSYS;
  va_end(copy);
  return ret;
}

void bringup_register_syscalls(void) {
  /* File syscalls (openat/newfstatat/readlinkat/lseek/mkdirat/unlinkat) are
   * registered by the real libc_init_file() against the FAT fs_server, do not
   * register them here (libc_define_syscall asserts the slot is still NULL). */
  libc_define_syscall(175, bringup_getid_zero); /* SYS_geteuid */
  libc_define_syscall(177, bringup_getid_zero); /* SYS_getegid */
  libc_define_syscall(SYS_sched_getaffinity, bringup_sched_getaffinity);
  libc_define_syscall(SYS_sched_setaffinity, bringup_zero);
  libc_define_syscall(SYS_uname, bringup_uname);
  libc_define_syscall(SYS_getcwd, bringup_getcwd);
  libc_define_syscall(SYS_clock_getres, bringup_clock_getres);
  libc_define_syscall(SYS_clock_nanosleep, bringup_clock_nanosleep);
  libc_define_syscall(SYS_sched_yield, bringup_zero);
  libc_define_syscall(SYS_rt_sigaction, bringup_zero);
  libc_define_syscall(SYS_rt_sigprocmask, bringup_zero);
  libc_define_syscall(SYS_sigaltstack, bringup_zero);
  libc_define_syscall(SYS_madvise, bringup_zero);
  libc_define_syscall(SYS_getrlimit, bringup_getrlimit);
  libc_define_syscall(SYS_prlimit64, bringup_prlimit);
  libc_define_syscall(SYS_epoll_create1, bringup_epoll_create);
  libc_define_syscall(SYS_epoll_ctl, bringup_epoll_ctl);
  libc_define_syscall(SYS_epoll_pwait, bringup_epoll_pwait);
  libc_define_syscall(SYS_pselect6, bringup_pselect6);
  /* ppoll stays with bringup (not sock.c): main.c nulls sock.c's poll callbacks
   * so sock.c does not claim __NR_ppoll, keeping the cothread-yielding stub
   * that ERTS boot needs. */
  libc_define_syscall(SYS_ppoll, bringup_poll_yield);
  libc_define_syscall(SYS_pipe2, bringup_pipe2);
  libc_define_syscall(SYS_timerfd_create, bringup_timerfd_create);
  libc_define_syscall(SYS_timerfd_settime, bringup_zero);
  libc_define_syscall(SYS_timerfd_gettime, bringup_zero);
  libc_define_syscall(SYS_tkill, bringup_zero);
  libc_define_syscall(SYS_tgkill, bringup_zero);
  libc_define_syscall(SYS_futex, bringup_futex);
  libc_define_syscall(SYS_prctl, bringup_zero); /* PR_SET_NAME etc.: cosmetic */
  libc_define_syscall(SYS_socketpair, bringup_socketpair);
  libc_define_syscall(SYS_exit, bringup_exit);
  libc_define_syscall(SYS_exit_group, bringup_exit);

  /* Real-entropy redefines: these slots are already claimed by libc_init /
   * libc_init_file, so REPLACE them (returning the old handler for chaining)
   * rather than define. rng_init() (main.c) seeds the DRBG right after this. */
  old_clock_gettime =
      libc_redefine_syscall(SYS_clock_gettime, bringup_clock_gettime);
  libc_redefine_syscall(SYS_getrandom, bringup_getrandom);
  old_openat = libc_redefine_syscall(SYS_openat, bringup_openat);

  /* Wire stdin (fd 0) to the serial RX queue. The libc fd.c initializes
   * stdin with O_RDONLY but no read callback, we provide the missing read
   * path by dequeuing from serial_rx_queue_handle. */
  fd_entry_t *stdin_entry = posix_fd_entry(STDIN_FILENO);
  if (stdin_entry != NULL) {
    stdin_entry->read = console_read;
  }
}
