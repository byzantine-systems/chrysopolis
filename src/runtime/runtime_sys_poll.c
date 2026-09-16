/*
 * epoll, ppoll and pselect6 over serial and socket readiness.
 *
 * ERTS's kernel-poll I/O subsystem watches stdin and its sockets through
 * epoll. Its fallback pollset thread and the schedulers use ppoll, and a few
 * shell paths use select. Two sources of readiness exist: stdin (fd 0) is
 * readable when the serial RX queue has data, and a socket descriptor reports
 * the state tcp.c keeps for it. Every other descriptor never becomes ready.
 *
 * All three wait the same way. They pump lwIP, scan, and if nothing is ready
 * and the timeout is nonzero they arm the timer multiplexer for a finite
 * timeout, park for ONE pulse, rescan and return. The call can return 0 before
 * its deadline. ERTS updates its pollset and timeout between calls and
 * announces a change by writing its wakeup pipe, which pulses the waiters
 * (runtime_fd_discard_write). Looping here until the original deadline would
 * miss, for example, a shorter timer set after the park. ERTS treats a 0 return
 * as a timeout and re-evaluates. The PD blocks in seL4_Recv while parked, so
 * QEMU's main loop runs freely and slirp's hostfwd SYNs arrive.
 *
 * There is one epoll table shared by every epoll instance. ERTS creates one.
 * Closing a descriptor leaves its epoll entry in place. A later ADD or MOD for
 * the reused descriptor number overwrites that entry.
 */
#include "runtime_config.h"
#include "runtime_console.h"
#include "runtime_deadline.h"
#include "runtime_fd.h"
#include "runtime_network.h"
#include "runtime_syscall_handlers.h"
#include "runtime_timer.h"
#include "runtime_wait.h"

#include <lions/posix/fd.h>
#include <lions/posix/posix.h>

#include <libmicrokitco.h>
#include <sddf/timer/client.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

/* socket_revents builds one event mask for both epoll and poll callers. */
static_assert(EPOLLIN == POLLIN);
static_assert(EPOLLOUT == POLLOUT);
static_assert(EPOLLERR == POLLERR);
static_assert(EPOLLHUP == POLLHUP);

static constexpr size_t epoll_max_fds = 64;

struct epoll_entry {
  int fd;
  /* Returned verbatim by epoll_pwait: ERTS identifies the I/O source by it. */
  struct epoll_event ev;
  bool active;
  /* EPOLLONESHOT: cleared after one report, set again by EPOLL_CTL_MOD. */
  bool armed;
  /* Readiness comes from tcp.c when set, otherwise only fd 0 can be ready. */
  bool is_sock;
  /* tcp.c socket index, valid when is_sock is set. */
  int sock_handle;
};

static struct epoll_entry epoll_table[epoll_max_fds];

/* True when fd is an open descriptor whose fstat reports S_IFSOCK. Safe for
 * any fd value. socket_index_of_fd indexes its table without a bounds check
 * and asserts the entry is a socket, so it may only be called after this
 * returns true. */
static bool fd_is_socket(int fd) {
  const fd_entry_t *e = posix_fd_entry(fd);
  struct stat st = {};
  return e != nullptr && e->fstat != nullptr && e->fstat(fd, &st) == 0 &&
         S_ISSOCK(st.st_mode);
}

/* Every readiness condition tcp.c reports for one socket. The callbacks only
 * read socket state. */
static uint32_t socket_revents(int handle) {
  uint32_t revents = 0;
  if (socket_config.tcp_socket_readable(handle)) {
    revents |= EPOLLIN;
  }
  if (socket_config.tcp_socket_writable(handle)) {
    revents |= EPOLLOUT;
  }
  if (socket_config.tcp_socket_hup(handle)) {
    revents |= EPOLLHUP;
  }
  if (socket_config.tcp_socket_err(handle) != 0) {
    revents |= EPOLLERR;
  }
  return revents;
}

/* Arm the timer multiplexer timeout_ns from now, before parking. */
static void arm_timeout(uint64_t timeout_ns) {
  beam_timer_arm(runtime_deadline_after(
      sddf_timer_time_now(timer_config.driver_id), timeout_ns));
}

long runtime_sys_epoll_create1(va_list ap) {
  (void)ap;
  return runtime_fd_alloc(runtime_fd_eof_read, runtime_fd_discard_write,
                          nullptr, O_RDWR);
}

long runtime_sys_epoll_ctl(va_list ap) {
  (void)va_arg(ap, long); /* epfd */
  const int op = runtime_sys_arg_int(va_arg(ap, long));
  const int fd = runtime_sys_arg_int(va_arg(ap, long));
  const struct epoll_event *event = runtime_sys_arg_pointer(va_arg(ap, long));

  if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
    if (event == nullptr) {
      return -EFAULT;
    }
    /* stdin, sockets and the synthetic descriptors are all open entries in the
     * libc table. Removal stays permissive: EPOLL_CTL_DEL of an unknown fd
     * succeeds, and ADD of a registered fd updates it. */
    if (posix_fd_entry(fd) == nullptr) {
      return -EBADF;
    }
    struct epoll_entry *entry = nullptr;
    for (size_t i = 0; i < epoll_max_fds; i++) {
      if (epoll_table[i].active && epoll_table[i].fd == fd) {
        entry = &epoll_table[i];
        break;
      }
    }
    if (entry == nullptr) {
      for (size_t i = 0; i < epoll_max_fds; i++) {
        if (!epoll_table[i].active) {
          entry = &epoll_table[i];
          break;
        }
      }
    }
    if (entry == nullptr) {
      return -ENOSPC;
    }
    const bool is_sock = fd_is_socket(fd);
    *entry = (struct epoll_entry){
        .fd = fd,
        .ev = *event,
        .active = true,
        .armed = true,
        .is_sock = is_sock,
        .sock_handle = is_sock ? socket_index_of_fd(fd) : -1,
    };
  } else if (op == EPOLL_CTL_DEL) {
    for (size_t i = 0; i < epoll_max_fds; i++) {
      if (epoll_table[i].active && epoll_table[i].fd == fd) {
        epoll_table[i].active = false;
        break;
      }
    }
  }
  return 0;
}

static int epoll_scan(struct epoll_event *events, int maxevents) {
  int n = 0;
  for (size_t i = 0; i < epoll_max_fds && n < maxevents; i++) {
    struct epoll_entry *entry = &epoll_table[i];
    if (!entry->active || !entry->armed) {
      continue;
    }
    uint32_t revents = 0;
    if (entry->is_sock) {
      revents = socket_revents(entry->sock_handle);
    } else if (entry->fd == 0 && runtime_console_readable()) {
      revents = EPOLLIN;
    }
    revents &= entry->ev.events | EPOLLERR | EPOLLHUP;
    if (revents == 0) {
      continue;
    }
    events[n].events = revents;
    events[n].data = entry->ev.data;
    n++;
    /* EPOLLONESHOT: stay silent until ERTS re-arms with EPOLL_CTL_MOD. This
     * keeps the scheduler from spinning on one event and lets the dirty-IO
     * cothread run read(0). */
    if (entry->ev.events & EPOLLONESHOT) {
      entry->armed = false;
    }
  }
  return n;
}

long runtime_sys_epoll_pwait(va_list ap) {
  (void)va_arg(ap, long); /* epfd */
  struct epoll_event *events = runtime_sys_arg_pointer(va_arg(ap, long));
  const int maxevents = runtime_sys_arg_int(va_arg(ap, long));
  const int timeout = runtime_sys_arg_int(va_arg(ap, long));

  if (events == nullptr || maxevents <= 0) {
    return -EINVAL;
  }

  beam_net_pump();

  int n = epoll_scan(events, maxevents);
  if (n == 0 && timeout != 0) {
    if (timeout > 0) {
      arm_timeout((uint64_t)timeout * 1000000);
    }
    thread_io_wait();
    beam_net_pump();
    n = epoll_scan(events, maxevents);
  }

  /* The dirty-IO scheduler cothread performs the deferred console read for
   * the prim_tty NIF, so it needs a turn after every poll. */
  microkit_cothread_yield();
  return n;
}

/* fds must hold nfds entries. */
static int poll_scan(struct pollfd *fds, nfds_t nfds) {
  int ready = 0;
  for (nfds_t i = 0; i < nfds; i++) {
    struct pollfd *p = &fds[i];
    p->revents = 0;
    if (p->fd < 0) {
      continue;
    }
    if (p->fd == 0) {
      if ((p->events & POLLIN) && runtime_console_readable()) {
        p->revents = POLLIN;
      }
    } else if (fd_is_socket(p->fd)) {
      /* Only requested IN/OUT conditions are reported. HUP and ERR always
       * are, as poll(2) specifies. */
      const uint32_t wanted =
          ((uint32_t)p->events & (POLLIN | POLLOUT)) | POLLHUP | POLLERR;
      /* The mask holds only the four low poll bits, so it fits in short. */
      p->revents = (short)(socket_revents(socket_index_of_fd(p->fd)) & wanted);
    }
    if (p->revents != 0) {
      ready++;
    }
  }
  return ready;
}

/* ppoll: ERTS's fallback pollset thread ppolls its wakeup pipe forever, and
 * the schedulers use it as a poll backstop. Returning immediately after a
 * yield would keep the fallback thread in a hot loop, so it parks like
 * epoll_pwait. */
long runtime_sys_ppoll(va_list ap) {
  struct pollfd *fds = runtime_sys_arg_pointer(va_arg(ap, long));
  const nfds_t nfds = (nfds_t)runtime_sys_arg_size(va_arg(ap, long));
  const struct timespec *tmo = runtime_sys_arg_pointer(va_arg(ap, long));

  if (fds == nullptr && nfds > 0) {
    return -EFAULT;
  }
  if (nfds > (nfds_t)runtime_fd_limit) {
    return -EINVAL;
  }
  if (tmo != nullptr && !runtime_timespec_valid(tmo)) {
    return -EINVAL;
  }

  beam_net_pump();

  int ready = poll_scan(fds, nfds);
  const bool zero_timeout =
      tmo != nullptr && tmo->tv_sec == 0 && tmo->tv_nsec == 0;
  if (ready == 0 && !zero_timeout) {
    if (tmo != nullptr) {
      arm_timeout(runtime_timespec_ns(tmo));
    }
    thread_io_wait();
    beam_net_pump();
    ready = poll_scan(fds, nfds);
  }

  microkit_cothread_yield();
  return ready;
}

/* pselect6: ERTS uses epoll for stdin, but other paths such as select()-based
 * timeout sleeps reach this. Only stdin readability is reported. The write
 * and exception sets are always returned empty. */
long runtime_sys_pselect6(va_list ap) {
  const int nfds = runtime_sys_arg_int(va_arg(ap, long));
  fd_set *readfds = runtime_sys_arg_pointer(va_arg(ap, long));
  fd_set *writefds = runtime_sys_arg_pointer(va_arg(ap, long));
  fd_set *exceptfds = runtime_sys_arg_pointer(va_arg(ap, long));
  const struct timespec *timeout = runtime_sys_arg_pointer(va_arg(ap, long));
  (void)va_arg(ap, long); /* sigmask */

  if (nfds < 0 || nfds > FD_SETSIZE) {
    return -EINVAL;
  }
  if (timeout != nullptr && !runtime_timespec_valid(timeout)) {
    return -EINVAL;
  }

  /* select examines only descriptors below nfds. */
  const bool want_stdin =
      nfds > 0 && readfds != nullptr && FD_ISSET(0, readfds);
  const bool zero_timeout =
      timeout != nullptr && timeout->tv_sec == 0 && timeout->tv_nsec == 0;

  int ready = want_stdin && runtime_console_readable() ? 1 : 0;
  if (ready == 0 && !zero_timeout) {
    beam_net_pump();
    if (timeout != nullptr) {
      arm_timeout(runtime_timespec_ns(timeout));
    }
    thread_io_wait();
    if (want_stdin && runtime_console_readable()) {
      ready = 1;
    }
  }

  if (readfds != nullptr) {
    FD_ZERO(readfds);
    if (ready) {
      FD_SET(0, readfds);
    }
  }
  if (writefds != nullptr) {
    FD_ZERO(writefds);
  }
  if (exceptfds != nullptr) {
    FD_ZERO(exceptfds);
  }
  return ready;
}
