#ifndef CHRYSOPOLIS_RUNTIME_FD_H
#define CHRYSOPOLIS_RUNTIME_FD_H 1

#include <lions/posix/fd.h>

#include <stddef.h>
#include <sys/types.h>

/*
 * Synthetic descriptors backed by callbacks instead of a server. Every
 * descriptor built here is owned by the libc descriptor table and released by
 * close(2) through runtime_fd_close.
 */

/* Discard the bytes and pulse idle waiters, so a wakeup-pipe write reaches
 * cothreads parked in the poll handlers. Always reports a full write. */
ssize_t runtime_fd_discard_write(const void *data, size_t count, int fd);

/* Report end of file without touching the buffer. */
ssize_t runtime_fd_eof_read(void *data, size_t count, int fd);

/* A read end that never receives data: -EAGAIN when the descriptor is
 * non-blocking, otherwise the calling cothread parks forever. */
ssize_t runtime_fd_empty_read(void *data, size_t count, int fd);

/*
 * RLIMIT_NOFILE as reported to ERTS, and the largest descriptor count ppoll
 * accepts. The libc table itself holds MAX_FDS entries.
 */
static constexpr int runtime_fd_limit = 1024;

/* Return the descriptor to the libc table. */
int runtime_fd_close(int fd);

/*
 * Allocate a descriptor with the given callbacks and open flags. A null read
 * or write callback, or a null fstat callback, becomes an operation that
 * returns -EBADF. close and dup3 get defaults that assume no per-open state
 * outside the libc fd entry; use runtime_fd_alloc_ops when there is some.
 * Returns the descriptor, fully initialised, or
 * -EMFILE with nothing allocated.
 */
[[__nodiscard__]] int runtime_fd_alloc(fd_read_func read, fd_write_func write,
                                       fd_fstat_func fstat, int flags);

/* Callbacks for a descriptor with per-open state outside its fd entry. A
 * null member gets runtime_fd_alloc's default. */
typedef struct {
  fd_read_func read;
  fd_write_func write;
  fd_fstat_func fstat;
  /* Must release the descriptor with runtime_fd_close. */
  fd_close_func close;
  /* sys_dup3 has already copied the entry into newfd; refusing must
   * deallocate newfd. */
  fd_dup3_func dup3;
} runtime_fd_ops;

/* runtime_fd_alloc with every callback chosen. Same result contract. */
[[__nodiscard__]] int runtime_fd_alloc_ops(const runtime_fd_ops *ops,
                                           int flags);

#endif
