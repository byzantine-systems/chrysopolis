#ifndef CHRYSOPOLIS_RUNTIME_PD_RESTART_PARSE_H
#define CHRYSOPOLIS_RUNTIME_PD_RESTART_PARSE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Request decoding for the test-only /dev/pd-restart trigger. Pure: the caller
 * owns the channel table and performs the notification.
 */

/* Operation rows of the channel table. The adapter asserts these against the
 * generated PD_RESTART_MODE_* values. */
enum : size_t {
  runtime_pd_restart_mode_healthy = 0,
  runtime_pd_restart_mode_fault = 1,
};

typedef enum {
  runtime_pd_restart_parse_ok,
  /* Empty after trimming, or no class name matches. */
  runtime_pd_restart_parse_unknown,
} runtime_pd_restart_parse_status;

/*
 * Decode a write payload: a class name for a healthy restart, or
 * "fault:<class>" for a fault injection. Trailing '\n', '\r', ' ' and '\t' are
 * ignored. buf need not be NUL-terminated and must hold count bytes (it may be
 * null when count is 0). names holds class_count non-null NUL-terminated
 * names. On ok, *mode and *class_index receive the operation row and the
 * matching index; on unknown both are left unwritten.
 */
[[__nodiscard__]] runtime_pd_restart_parse_status
runtime_pd_restart_parse(const char *buf, size_t count,
                         const char *const names[static 1], size_t class_count,
                         size_t *mode, size_t *class_index);

/* True when any entry of the modes x classes channel table differs from none,
 * which is how a restart image differs from a production image. */
[[__nodiscard__]] bool runtime_pd_restart_any_channel(
    size_t modes, size_t classes,
    const volatile uint8_t channels[static modes][classes], uint8_t none);

#endif
