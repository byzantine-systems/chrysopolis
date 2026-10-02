#ifndef CHRYSOPOLIS_LIB_ABI_ROOT_STATUS_READER_H
#define CHRYSOPOLIS_LIB_ABI_ROOT_STATUS_READER_H 1

/* Reader side of Root's status page. It depends only on the generated wire
 * contract, so the BEAM links it without any of Root's policy code. */
#include <chrysopolis/root_control.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  root_status_read_ok,
  /* No stable even sequence within the attempt budget. */
  root_status_read_torn,
  /* A stable copy that fails the generated checker. */
  root_status_read_invalid,
  /* Bad buffers or length; nothing was read. */
  root_status_read_size,
} root_status_read_result;

/* One attempt, split so a test can act as the writer between the halves.
 * begin copies the page into scratch when its sequence is published and even,
 * and returns false otherwise. finish rereads the sequence and, only when it
 * is unchanged and the copy validates, copies scratch to output. These two
 * are unchecked: null or overlapping buffers are undefined behavior. Use
 * root_status_read, which validates its arguments, everywhere else. */
[[__nodiscard__]] bool
root_status_copy_begin(const chryso_root_status_page *shared,
                       chryso_root_status_page *scratch, uint64_t *seq);
[[__nodiscard__]] root_status_read_result
root_status_copy_finish(const chryso_root_status_page *shared,
                        const chryso_root_status_page *scratch,
                        chryso_root_status_page *output, uint64_t seq);

/* Up to `attempts` copies; zero attempts is torn. Returns size, reading
 * nothing, for a null or aliased buffer or len != sizeof(page). output is
 * unchanged unless the result is ok. Not thread-safe per buffer pair. */
[[__nodiscard__]] root_status_read_result root_status_read(
    const chryso_root_status_page *shared, chryso_root_status_page *scratch,
    chryso_root_status_page *output, size_t len, unsigned int attempts);

/* Events this reader lost since it last saw previous_head. This is distinct
 * from the page's event_dropped, Root's global overwrite count. */
[[__nodiscard__]] uint64_t root_status_missed(uint64_t previous_head,
                                              uint64_t current_head);

typedef enum {
  root_status_observe_ok,
  /* Within one Root generation a counter went backwards, or an equal seq
   * carried different bytes. */
  root_status_observe_regressed,
} root_status_observe_result;

typedef struct {
  uint64_t missed_events;
  uint64_t age_ticks;
  bool age_known;
  bool changed;
} root_status_freshness;

/* Compares two validated copies. A quiet unchanged page remains valid; its age
 * is a sampled observation, never a health or readiness verdict. prior may be
 * null on the first read; a null current or out is reported as regressed.
 * out is written only on ok. */
[[__nodiscard__]] root_status_observe_result
root_status_observe(const chryso_root_status_page *prior,
                    const chryso_root_status_page *current,
                    uint64_t reader_ticks, uint64_t reader_frequency,
                    root_status_freshness *out);

#endif
