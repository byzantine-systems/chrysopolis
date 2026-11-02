/*
 * pipe2, timerfd_create and socketpair for ERTS-internal plumbing.
 *
 * None of these carries application data. Networking goes through the libc
 * socket layer (sock.c, tcp.c, lwIP), and readiness comes from the serial RX
 * queue and tcp.c socket state.
 *
 * pipe2: ERTS builds self-pipes to wake its poll set and its signal
 * dispatcher. Writes are counted, not stored (runtime_wake_pipe.h), so the
 * read end stays readable until drained: erts_poll_wait leaves its wait only
 * when the pipe reads as ready. Each write also pulses idle waiters.
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
#include "runtime_fd_pair.h"
#include "runtime_syscall_handlers.h"
#include "runtime_wait.h"
#include "runtime_wake_pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>

/* ERTS opens about three self-pipes; the rest is headroom. */
static constexpr size_t pipe_slots = 8;
static runtime_pipe_slot pipes[pipe_slots];

static ssize_t pipe_errno(runtime_pipe_status status, size_t bytes) {
  switch (status) {
  case runtime_pipe_ok:
    return (ssize_t)bytes;
  case runtime_pipe_would_block:
    return -EAGAIN;
  case runtime_pipe_eof:
    return 0;
  case runtime_pipe_broken:
    return -EPIPE;
  case runtime_pipe_no_slot:
    return -ENFILE;
  case runtime_pipe_unknown_fd:
    return -EBADF;
  }
  return -EIO;
}

static ssize_t pipe_read(void *data, size_t count, int fd) {
  for (;;) {
    size_t taken = 0;
    const runtime_pipe_status status =
        runtime_pipe_read(pipes, pipe_slots, fd, count, &taken);
    const fd_entry_t *e = posix_fd_entry(fd);
    if (status != runtime_pipe_would_block ||
        (e != nullptr && (e->flags & O_NONBLOCK))) {
      if (taken != 0) {
        memset(data, 0, taken);
      }
      return pipe_errno(status, taken);
    }
    /* ERTS's signal dispatcher blocks here; park until a write. */
    thread_io_wait();
  }
}

static ssize_t pipe_write(const void *data, size_t count, int fd) {
  (void)data;
  size_t accepted = 0;
  const runtime_pipe_status status =
      runtime_pipe_write(pipes, pipe_slots, fd, count, &accepted);
  thread_io_wake();
  return pipe_errno(status, accepted);
}

static int pipe_close(int fd) {
  (void)runtime_pipe_close(pipes, pipe_slots, fd);
  return runtime_fd_close(fd);
}

/* A duplicate would be a second name the table cannot resolve. */
static int pipe_dup3(int oldfd, int newfd) {
  (void)oldfd;
  (void)runtime_fd_close(newfd);
  return -EBUSY;
}

bool runtime_pipe_fd_ready(int fd) {
  return runtime_pipe_ready(pipes, pipe_slots, fd);
}

/* The pipe2 flags for the read end. */
typedef struct {
  int flags;
} pipe_context;

static int pipe_alloc(void *context, size_t which) {
  const pipe_context *request = context;
  const int nonblock = request->flags & O_NONBLOCK;
  if (which == 0) {
    return runtime_fd_alloc_ops(&(const runtime_fd_ops){.read = pipe_read,
                                                        .close = pipe_close,
                                                        .dup3 = pipe_dup3},
                                O_RDONLY | nonblock);
  }
  return runtime_fd_alloc_ops(&(const runtime_fd_ops){.write = pipe_write,
                                                      .close = pipe_close,
                                                      .dup3 = pipe_dup3},
                              O_WRONLY | nonblock);
}

static int socketpair_alloc(void *context, size_t which) {
  (void)context;
  (void)which;
  return runtime_fd_alloc(runtime_fd_empty_read, runtime_fd_discard_write,
                          nullptr, O_RDWR | O_NONBLOCK);
}

static int pair_close(void *context, int fd) {
  (void)context;
  return runtime_fd_close(fd);
}

long runtime_sys_pipe2(va_list ap) {
  int *pipefd = runtime_sys_arg_pointer(va_arg(ap, long));
  pipe_context request = {.flags = runtime_sys_arg_int(va_arg(ap, long))};
  if (pipefd == nullptr) {
    return -EFAULT;
  }
  const runtime_fd_pair_ops ops = {
      .alloc = pipe_alloc, .close = pair_close, .context = &request};
  int fds[2] = {};
  const int status = runtime_fd_alloc_pair(&ops, fds);
  if (status != 0) {
    return status;
  }
  if (runtime_pipe_open(pipes, pipe_slots, fds[0], fds[1]) != runtime_pipe_ok) {
    (void)runtime_fd_close(fds[0]);
    (void)runtime_fd_close(fds[1]);
    return -ENFILE;
  }
  pipefd[0] = fds[0];
  pipefd[1] = fds[1];
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
  const runtime_fd_pair_ops ops = {
      .alloc = socketpair_alloc, .close = pair_close, .context = nullptr};
  return runtime_fd_alloc_pair(&ops, sv);
}
