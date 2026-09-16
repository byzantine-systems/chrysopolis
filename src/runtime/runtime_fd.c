/*
 * Synthetic descriptors for ERTS-internal plumbing.
 *
 * ERTS creates pipes, timerfds and socketpairs for its own coordination. None
 * of them carries data here, so their descriptors are callback-backed entries
 * in the libc table: writes are discarded and reads see EOF or an always-empty
 * queue.
 *
 * A discarded write still wakes idle waiters. ERTS wakes its pollset by
 * writing a byte to its self-pipe, and the poller parked in epoll_pwait or
 * ppoll must return so ERTS re-evaluates its pollset and timeout. On Linux the
 * pipe becoming readable does that. Here the thread_io_wake pulse does.
 */
#include "runtime_fd.h"
#include "runtime_wait.h"

#include <errno.h>
#include <fcntl.h>

/* LionsOS io.c validates only that an fd entry exists before calling its
 * operation pointer. Keep every entry total so an unsupported operation is an
 * ordinary syscall error instead of an indirect call through nullptr. */
static ssize_t runtime_fd_bad_write(const void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  (void)fd;
  return -EBADF;
}

static ssize_t runtime_fd_bad_read(void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  (void)fd;
  return -EBADF;
}

/* sys_dup3 copies the fd entry before invoking this callback. Synthetic
 * descriptors hold all their state in that entry, so the copy is complete. */
static int runtime_fd_dup3(int oldfd, int newfd) {
  (void)oldfd;
  (void)newfd;
  return 0;
}

static int runtime_fd_bad_fstat(int fd, struct stat *st) {
  (void)fd;
  (void)st;
  return -EBADF;
}

ssize_t runtime_fd_discard_write(const void *data, size_t count, int fd) {
  (void)data;
  (void)fd;
  thread_io_wake();
  return (ssize_t)count;
}

ssize_t runtime_fd_eof_read(void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  (void)fd;
  return 0;
}

ssize_t runtime_fd_empty_read(void *data, size_t count, int fd) {
  (void)data;
  (void)count;
  const fd_entry_t *e = posix_fd_entry(fd);
  if (e != nullptr && (e->flags & O_NONBLOCK)) {
    return -EAGAIN;
  }
  /* ERTS's signal-dispatcher thread blocks here reading its signal pipe. There
   * are no OS signals, so no byte ever arrives. Parking consumes no CPU, which
   * lets the PD idle where a yield loop would keep it busy. */
  thread_park_forever();
}

int runtime_fd_close(int fd) { return posix_fd_deallocate(fd); }

int runtime_fd_alloc(fd_read_func read, fd_write_func write,
                     fd_fstat_func fstat, int flags) {
  const int fd = posix_fd_allocate();
  if (fd < 0) {
    return -EMFILE;
  }
  fd_entry_t *e = posix_fd_entry(fd);
  if (e == nullptr) {
    /* posix_fd_allocate marked the slot active, so give it back. */
    (void)posix_fd_deallocate(fd);
    return -EMFILE;
  }
  /* posix_fd_allocate only marks the slot active. Write the whole entry so no
   * field (dup3, file_ptr) depends on how a previous owner released it. */
  *e = (fd_entry_t){
      .read = read != nullptr ? read : runtime_fd_bad_read,
      .write = write != nullptr ? write : runtime_fd_bad_write,
      .close = runtime_fd_close,
      .dup3 = runtime_fd_dup3,
      .fstat = fstat != nullptr ? fstat : runtime_fd_bad_fstat,
      .flags = flags,
  };
  return fd;
}
