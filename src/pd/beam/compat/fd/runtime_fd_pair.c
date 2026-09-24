/*
 * Pair allocation for ERTS's self-pipe and port-forker socketpair. The first
 * descriptor is owned here until the second exists, so a table that fills up
 * between the two allocations does not leak a slot.
 */
#include "runtime_fd_pair.h"

int runtime_fd_alloc_pair(const runtime_fd_pair_ops *ops, int out[static 2]) {
  const int first = ops->alloc(ops->context, 0);
  if (first < 0) {
    return first;
  }
  const int second = ops->alloc(ops->context, 1);
  if (second < 0) {
    (void)ops->close(ops->context, first);
    return second;
  }
  out[0] = first;
  out[1] = second;
  return 0;
}
