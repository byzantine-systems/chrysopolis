/*
 * Bring-up syscall shims for the beam_server PD.
 *
 * The LionsOS libc only registers the syscalls its own components need; ERTS
 * reaches for a number more during boot (epoll/pselect6/ppoll for its poll set,
 * pipe2/timerfd/socketpair, a cothread-aware futex, sched_getaffinity for the
 * CPU count, uname, clock_nanosleep, signal and scheduler no-ops). libc leaves
 * those slots NULL -> sel4_vsyscall returns -ENOSYS and ERTS spins, so
 * bringup_register_syscalls() installs benign stubs (called after libc_init,
 * while the slots are still free, so no double-register assert).
 *
 * File I/O (open/read/write/stat/lseek) is NOT handled here: the real libc fs
 * path (lib/libc/posix/file.c, wired in main.c) routes it to the FAT fs_server.
 */
#include <lions/posix/fd.h>
#include <lions/posix/posix.h>

#include <microkit.h>
#include <sel4/sel4.h>

#include <libmicrokitco.h>
#include <sddf/serial/config.h>
#include <sddf/serial/queue.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Serial RX queue handle for console input, defined in main.c */
extern serial_queue_handle_t serial_rx_queue_handle;
extern serial_client_config_t serial_config;
/* Timer client config (driver_id channel), defined in main.c. Used by
 * clock_nanosleep to read the monotonic clock for a real timed sleep. */
extern timer_client_config_t timer_config;

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
 * non-blocking fd reports EAGAIN (ERTS's poll wakeup pipe drains this way); a
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

/* Console read: dequeue characters from the serial RX queue into the user
 * buffer. For blocking reads (stdin default), yields the cothread when the
 * queue is empty to let the RX virtualiser refill it. For non-blocking reads
 * (O_NONBLOCK set via fcntl), returns -EAGAIN when empty. */
static ssize_t console_read(void *data, size_t count, int fd) {
  char *buf = (char *)data;
  size_t n = 0;

  fd_entry_t *e = posix_fd_entry(fd);
  bool nonblock = (e != NULL && (e->flags & O_NONBLOCK));

  while (n < count) {
    char c;
    if (serial_dequeue(&serial_rx_queue_handle, &c) == 0) {
      buf[n++] = c;
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

/* pipe2: ERTS builds a self-pipe to wake its poll set. The write end discards
 * and the read end is always empty (no real wakeups needed until the poll set
 * is integrated with microkit notifications). */
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

/* timerfd: ERTS was cross-built with timerfd support. Hand back a non-blocking
 * fd that never reports expirations. */
static long bringup_timerfd_create(va_list ap) {
  (void)ap;
  return bringup_alloc_fd(empty_read, devnull_write, O_RDWR | O_NONBLOCK);
}

/* socketpair: ERTS's spawn_init opens a unix-domain socketpair to talk to its
 * port forker (erl_child_setup). We cannot spawn OS processes, but spawn_init
 * must succeed for ERTS to boot, so we hand back two valid non-blocking fds.
 * Port operations over them simply never complete. */
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
 * word changes or a bounded number of rounds elapse (spurious return; ERTS
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
  for (int i = 0; i < 4096; i++) {
    microkit_cothread_yield();
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
      return 0;
    }
  }
  return 0; /* spurious; the caller re-checks */
}

/* musl's _Exit() calls exit_group then loops on exit; neither is implemented
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

/* ERTS reads the CPU affinity mask to size its scheduler pool; report a
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

/* ERTS queries the monotonic clock resolution; report a 1ns granularity. */
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
  uint64_t target =
      (flags & TIMER_ABSTIME) ? reqns : sddf_timer_time_now(ch) + reqns;
  while (sddf_timer_time_now(ch) < target) {
    microkit_cothread_yield();
  }
  if (rem != NULL && !(flags & TIMER_ABSTIME)) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

/* ERTS queries RLIMIT_NOFILE to size its fd tables; report a fixed limit. */
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
  bool armed; /* EPOLLONESHOT: disarmed after one report, re-armed by MOD */
} epoll_table[EPOLL_MAX_FDS];

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

/* epoll_pwait: scan registered fds for readiness. fd 0 (stdin) is readable
 * when the serial RX queue has data. Return matching events with the caller's
 * registered data. Yield when nothing is ready so helper cothreads run. */
static long bringup_epoll_pwait(va_list ap) {
  int epfd = va_arg(ap, int);
  struct epoll_event *events = va_arg(ap, struct epoll_event *);
  int maxevents = va_arg(ap, int);
  int timeout = va_arg(ap, int);
  (void)epfd;
  (void)timeout;

  if (events == NULL || maxevents <= 0) {
    return -EINVAL;
  }

  int n = 0;
  for (int i = 0; i < EPOLL_MAX_FDS && n < maxevents; i++) {
    if (!epoll_table[i].active || !epoll_table[i].armed) {
      continue;
    }
    uint32_t revents = 0;
    if (epoll_table[i].fd == 0) {
      if (serial_queue_length_consumer(&serial_rx_queue_handle) > 0) {
        revents |= EPOLLIN;
      }
    }
    revents &= (epoll_table[i].ev.events | EPOLLERR | EPOLLHUP);
    if (revents) {
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

  /* Always yield so the dirty-IO scheduler cothread (which performs the
   * deferred read via the prim_tty NIF) gets a chance to run. */
  microkit_cothread_yield();
  return n;
}

/* epoll_pwait / ppoll: ERTS's scheduler polls for I/O when idle. Returning
 * immediately would busy-spin the scheduler cothread and starve the helper
 * cothreads (aux, poll, dirty-I/O), stalling boot. Yield first so the
 * cothread scheduler runs the others, then report no ready events. */
static long bringup_poll_yield(va_list ap) {
  (void)ap;
  microkit_cothread_yield();
  return 0;
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

  /* If nothing ready, yield to other cothreads before returning */
  if (ready == 0) {
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

  /* Wire stdin (fd 0) to the serial RX queue. The libc fd.c initializes
   * stdin with O_RDONLY but no read callback; we provide the missing read
   * path by dequeuing from serial_rx_queue_handle. */
  fd_entry_t *stdin_entry = posix_fd_entry(STDIN_FILENO);
  if (stdin_entry != NULL) {
    stdin_entry->read = console_read;
  }
}
