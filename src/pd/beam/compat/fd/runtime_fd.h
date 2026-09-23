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
 * returns -EBADF. close and dup3 are always installed; duplicating these
 * callback-backed descriptors is safe because they have no per-open state
 * outside the libc fd entry. Returns the descriptor, fully initialised, or
 * -EMFILE with nothing allocated.
 */
[[__nodiscard__]] int runtime_fd_alloc(fd_read_func read, fd_write_func write,
                                       fd_fstat_func fstat, int flags);

#endif
