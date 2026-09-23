#ifndef CHRYSOPOLIS_RUNTIME_EPOLL_TABLE_H
#define CHRYSOPOLIS_RUNTIME_EPOLL_TABLE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The epoll registration table and readiness masks behind the poll syscall
 * handlers. Pure: the table is caller storage, readiness and reporting go
 * through callbacks, and nothing here blocks or touches a descriptor.
 */

/* Linux epoll and poll bit values. The two families share the low bits, and
 * the syscall adapter asserts both against the libc headers. */
enum : uint32_t {
  runtime_epoll_in = 0x001,
  runtime_epoll_out = 0x004,
  runtime_epoll_err = 0x008,
  runtime_epoll_hup = 0x010,
  runtime_epoll_oneshot = 1u << 30,
};

typedef struct {
  int fd;
  /* Requested events, returned conditions are filtered by these. */
  uint32_t events;
  /* epoll_data as its 64-bit representation, reported back verbatim: ERTS
   * identifies the I/O source by it. */
  uint64_t data;
  bool active;
  /* EPOLLONESHOT: cleared after one report, set again by the next set. */
  bool armed;
  /* Readiness comes from the socket layer when set. */
  bool is_sock;
  /* Socket index, meaningful only when is_sock is set. */
  int sock_handle;
} runtime_epoll_entry;

typedef enum {
  runtime_epoll_ok,
  /* No entry holds fd and no inactive entry is left. */
  runtime_epoll_no_space,
} runtime_epoll_status;

/*
 * EPOLL_CTL_ADD and EPOLL_CTL_MOD. Registers fd, or replaces an existing
 * registration of it (ADD of a registered fd updates it). The entry is armed.
 * On no_space the table is unchanged. table must hold capacity entries.
 */
[[__nodiscard__]] runtime_epoll_status
runtime_epoll_table_set(runtime_epoll_entry table[], size_t capacity, int fd,
                        uint32_t events, uint64_t data, bool is_sock,
                        int sock_handle);

/* EPOLL_CTL_DEL. Removing an fd that is not registered succeeds silently. */
void runtime_epoll_table_remove(runtime_epoll_entry table[], size_t capacity,
                                int fd);

typedef struct {
  /* Current conditions for one active, armed entry. */
  uint32_t (*readiness)(void *context, const runtime_epoll_entry *entry);
  /* Receive report number index (0, 1, ...) of this scan. */
  void (*report)(void *context, size_t index, uint32_t events, uint64_t data);
  void *context;
} runtime_epoll_scan_ops;

/*
 * Report up to max_reports ready entries in table order and return how many
 * were reported. Conditions are filtered to the requested events plus ERR and
 * HUP, which are always reported. A reported EPOLLONESHOT entry is disarmed
 * until the next set. ops and its callbacks must be non-null.
 */
[[__nodiscard__]] size_t
runtime_epoll_table_scan(runtime_epoll_entry table[], size_t capacity,
                         const runtime_epoll_scan_ops *ops, size_t max_reports);

/* The conditions a poll(2) entry may report: the requested IN and OUT bits,
 * and HUP and ERR unconditionally. */
[[__nodiscard__]] static inline uint32_t
runtime_poll_wanted_mask(uint32_t requested) {
  return (requested & (runtime_epoll_in | runtime_epoll_out)) |
         runtime_epoll_hup | runtime_epoll_err;
}

/* True when select's nfds lies in [0, setsize]. */
[[__nodiscard__]] static inline bool runtime_select_nfds_valid(int nfds,
                                                               int setsize) {
  return nfds >= 0 && nfds <= setsize;
}

/* One descriptor's readiness, split the way select(2) reports it. */
typedef struct {
  bool read;
  bool write;
  bool except;
} runtime_select_ready;

/*
 * Map the epoll-shaped conditions a descriptor reports onto select's three
 * sets. A descriptor with a pending error or a hangup is both readable and
 * writable: that is how select tells the caller to go and collect the error
 * rather than block on a descriptor that will never make progress.
 *
 * The except set stays empty. select reports an exceptional condition for
 * out-of-band data, which this stack's TCP does not deliver, and reporting
 * errors there instead would leave a caller that watches only readfds and
 * writefds blocked forever.
 */
[[__nodiscard__]] static inline runtime_select_ready
runtime_select_from_revents(uint32_t revents) {
  const bool failed = (revents & (runtime_epoll_err | runtime_epoll_hup)) != 0;
  return (runtime_select_ready){
      .read = (revents & runtime_epoll_in) != 0 || failed,
      .write = (revents & runtime_epoll_out) != 0 || failed,
      .except = false,
  };
}

#endif
