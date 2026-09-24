/*
 * One table serves every epoll instance, since ERTS creates one. Closing a
 * descriptor leaves its entry in place; a later set for the reused number
 * overwrites it.
 */
#include "runtime_epoll_table.h"

static runtime_epoll_entry *find_entry(runtime_epoll_entry table[],
                                       size_t capacity, int fd) {
  for (size_t i = 0; i < capacity; i++) {
    if (table[i].active && table[i].fd == fd) {
      return &table[i];
    }
  }
  return nullptr;
}

runtime_epoll_status runtime_epoll_table_set(runtime_epoll_entry table[],
                                             size_t capacity, int fd,
                                             uint32_t events, uint64_t data,
                                             bool is_sock, int sock_handle) {
  runtime_epoll_entry *entry = find_entry(table, capacity, fd);
  for (size_t i = 0; entry == nullptr && i < capacity; i++) {
    if (!table[i].active) {
      entry = &table[i];
    }
  }
  if (entry == nullptr) {
    return runtime_epoll_no_space;
  }
  *entry = (runtime_epoll_entry){
      .fd = fd,
      .events = events,
      .data = data,
      .active = true,
      .armed = true,
      .is_sock = is_sock,
      .sock_handle = is_sock ? sock_handle : -1,
  };
  return runtime_epoll_ok;
}

void runtime_epoll_table_remove(runtime_epoll_entry table[], size_t capacity,
                                int fd) {
  runtime_epoll_entry *entry = find_entry(table, capacity, fd);
  if (entry != nullptr) {
    entry->active = false;
  }
}

size_t runtime_epoll_table_scan(runtime_epoll_entry table[], size_t capacity,
                                const runtime_epoll_scan_ops *ops,
                                size_t max_reports) {
  size_t reported = 0;
  for (size_t i = 0; i < capacity && reported < max_reports; i++) {
    runtime_epoll_entry *entry = &table[i];
    if (!entry->active || !entry->armed) {
      continue;
    }
    const uint32_t conditions =
        ops->readiness(ops->context, entry) &
        (entry->events | runtime_epoll_err | runtime_epoll_hup);
    if (conditions == 0) {
      continue;
    }
    ops->report(ops->context, reported, conditions, entry->data);
    reported++;
    /* Stay silent until ERTS re-arms with EPOLL_CTL_MOD. This keeps the
     * scheduler from spinning on one event and lets the dirty-IO cothread run
     * read(0). */
    if (entry->events & runtime_epoll_oneshot) {
      entry->armed = false;
    }
  }
  return reported;
}
