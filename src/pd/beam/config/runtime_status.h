#ifndef CHRYSOPOLIS_RUNTIME_STATUS_H
#define CHRYSOPOLIS_RUNTIME_STATUS_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Ordered boot results for the beam_server PD and the policy attached to them.
 * Values stay within the low byte because recoverable failures are reported
 * through beam_request_restart. They are internal diagnostics, not generated
 * ABI. Pure: usable before libc and on the host test harness.
 */
typedef enum {
  RUNTIME_STATUS_OK = 0,
  RUNTIME_STATUS_CONFIG_SERIAL = 1,
  RUNTIME_STATUS_CONFIG_TIMER = 2,
  RUNTIME_STATUS_CONFIG_FS = 3,
  RUNTIME_STATUS_CONFIG_NET = 4,
  RUNTIME_STATUS_CONFIG_LWIP = 5,
  RUNTIME_STATUS_CONFIG_NET_PAIR = 6,
  RUNTIME_STATUS_COTHREAD_ALLOC = 32,
  RUNTIME_STATUS_FS_COMMAND = 33,
  RUNTIME_STATUS_FS_MOUNT = 34,
  RUNTIME_STATUS_CLOCK = 35,
  RUNTIME_STATUS_ENVIRONMENT = 36,
  RUNTIME_STATUS_PAYLOAD_SPAWN = 37,
  RUNTIME_STATUS_PROBE_SPAWN = 38,
  RUNTIME_STATUS_THREAD_PROBE = 39,
} runtime_status_t;

/* True for a configuration failure. A restart re-runs the same image over the
 * same patched blobs and cannot repair one, so the PD parks instead. */
[[__nodiscard__]] static inline bool
runtime_status_is_config_failure(runtime_status_t status) {
  return status >= RUNTIME_STATUS_CONFIG_SERIAL &&
         status <= RUNTIME_STATUS_CONFIG_NET_PAIR;
}

/* The exit code Root receives for a status: its low byte. */
[[__nodiscard__]] static inline uint8_t
runtime_status_exit_code(runtime_status_t status) {
  return (uint8_t)((unsigned)status & 0xffu);
}

/* Boot stage a status belongs to, as a static string. Never null; a value
 * outside the enum yields "unknown". */
[[__nodiscard__]] const char *runtime_status_stage(runtime_status_t status);

/* Stable diagnostic name of a status, as a static string. Never null; a value
 * outside the enum yields "unknown". */
[[__nodiscard__]] const char *runtime_status_name(runtime_status_t status);

/* Buffer size runtime_status_format_code needs: three digits and a NUL. */
static constexpr size_t runtime_status_code_text_size = 4;

/* Write the decimal exit code of status, NUL-terminated, into out and return
 * the digit count (1 to 3). */
size_t
runtime_status_format_code(runtime_status_t status,
                           char out[static runtime_status_code_text_size]);

#endif
