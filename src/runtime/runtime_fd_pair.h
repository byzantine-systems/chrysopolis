#ifndef CHRYSOPOLIS_RUNTIME_FD_PAIR_H
#define CHRYSOPOLIS_RUNTIME_FD_PAIR_H 1

#include <stddef.h>

/*
 * Two-descriptor allocation with rollback, for pipe2 and socketpair. Pure: the
 * descriptor table is reached only through the callbacks.
 */

typedef struct {
  /* Allocate descriptor `which` (0, then 1) of the pair. Returns the
   * descriptor, or a negative value that the pair operation returns as is. */
  int (*alloc)(void *context, size_t which);
  /* Release a descriptor this pair allocated. The result is ignored. */
  int (*close)(void *context, int fd);
  void *context;
} runtime_fd_pair_ops;

/*
 * Allocate both descriptors through ops and store them in out[0] and out[1].
 * Returns 0 on success. If either allocation fails, the first descriptor (when
 * it exists) is closed again, out is left unwritten, and the failing
 * allocation's negative result is returned. ops, its callbacks and out must be
 * non-null; context is passed through and never retained.
 */
[[__nodiscard__]] int runtime_fd_alloc_pair(const runtime_fd_pair_ops *ops,
                                            int out[static 2]);

#endif
