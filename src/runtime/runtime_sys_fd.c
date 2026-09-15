/*
 * pipe2, timerfd_create and socketpair for ERTS-internal plumbing.
 *
 * None of these carries application data. Networking goes through the libc
 * socket layer (sock.c, tcp.c, lwIP), and readiness comes from the serial RX
 * queue and tcp.c socket state.
 *
 * pipe2: ERTS builds a self-pipe to wake its poll set from a signal or aux
 * thread. The write end discards and pulses idle waiters, and the read end is
 * always empty.
 *
 * timerfd_create: the emulator was cross-built with timerfd support for its
 * time-correction machinery. The descriptor never reports an expiration. The
 * sDDF timer drives the monotonic clock behind clock_gettime and
 * clock_nanosleep.
 *
 * socketpair: ERTS's spawn_init opens an AF_UNIX pair to talk to its port
 * forker (erl_child_setup). OS processes cannot be spawned, but spawn_init has
 * to succeed for ERTS to boot, so this hands back two valid non-blocking
 * descriptors on which port operations never complete. sock.c does not
 * implement socketpair.
 */
#include "runtime_fd.h"
#include "runtime_syscall_handlers.h"

#include <errno.h>
#include <fcntl.h>

long runtime_sys_pipe2(va_list ap) {
  int *pipefd = runtime_sys_arg_pointer(va_arg(ap, long));
  const int flags = runtime_sys_arg_int(va_arg(ap, long));
  if (pipefd == nullptr) {
    return -EFAULT;
  }
  /* Keep O_NONBLOCK so the read end blocks or returns -EAGAIN as asked. */
  const int rfd = runtime_fd_alloc(runtime_fd_empty_read, nullptr, nullptr,
                                   O_RDONLY | (flags & O_NONBLOCK));
  if (rfd < 0) {
    return rfd;
  }
  const int wfd =
      runtime_fd_alloc(nullptr, runtime_fd_discard_write, nullptr, O_WRONLY);
  if (wfd < 0) {
    (void)runtime_fd_close(rfd);
    return wfd;
  }
  pipefd[0] = rfd;
  pipefd[1] = wfd;
  return 0;
}

long runtime_sys_timerfd_create(va_list ap) {
  (void)ap;
  return runtime_fd_alloc(runtime_fd_empty_read, runtime_fd_discard_write,
                          nullptr, O_RDWR | O_NONBLOCK);
}

long runtime_sys_socketpair(va_list ap) {
  (void)va_arg(ap, long); /* domain */
  (void)va_arg(ap, long); /* type */
  (void)va_arg(ap, long); /* protocol */
  int *sv = runtime_sys_arg_pointer(va_arg(ap, long));
  if (sv == nullptr) {
    return -EFAULT;
  }
  const int a =
      runtime_fd_alloc(runtime_fd_empty_read, runtime_fd_discard_write, nullptr,
                       O_RDWR | O_NONBLOCK);
  if (a < 0) {
    return a;
  }
  const int b =
      runtime_fd_alloc(runtime_fd_empty_read, runtime_fd_discard_write, nullptr,
                       O_RDWR | O_NONBLOCK);
  if (b < 0) {
    (void)runtime_fd_close(a);
    return b;
  }
  sv[0] = a;
  sv[1] = b;
  return 0;
}
