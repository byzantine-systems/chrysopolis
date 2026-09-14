#ifndef CHRYSOPOLIS_RUNTIME_LIFECYCLE_H
#define CHRYSOPOLIS_RUNTIME_LIFECYCLE_H 1

#include <microkit.h>

/*
 * Ordered boot results for the beam_server PD. Values stay within the low
 * byte because recoverable failures are reported through beam_request_restart.
 * They are internal diagnostics, not generated ABI.
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
} runtime_status_t;

/*
 * Start every subsystem in dependency order. A failure leaves control on the
 * root context and must be passed to runtime_lifecycle_fail; the caller must
 * not enter the Microkit event loop with partially initialised state.
 */
[[nodiscard]] runtime_status_t runtime_lifecycle_start(void);

/* Dispatch one Microkit notification in the established runtime order. */
void runtime_lifecycle_notified(microkit_channel ch);

/*
 * Configuration failures log and park because a restart cannot repair the
 * image. Later failures ask Root for a bounded restart. Never returns.
 */
[[noreturn]] void runtime_lifecycle_fail(runtime_status_t status);

#endif
