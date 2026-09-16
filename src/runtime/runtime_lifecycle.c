/*
 * Ordered lifecycle coordinator for beam_server.
 * Subsystems own their state; this file records only startup dependencies and
 * Microkit event ordering.
 */
#include "runtime_lifecycle.h"
#include "rng.h"
#include "runtime_boot.h"
#include "runtime_config.h"
#include "runtime_cothread.h"
#include "runtime_fs.h"
#include "runtime_network.h"
#include "runtime_restart.h"
#include "runtime_syscalls.h"
#include "runtime_timer.h"
#include "runtime_wait.h"

#include <lions/posix/posix.h>

#include <libmicrokitco.h>
#include <microkit.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

static libc_socket_config_t beam_socket_config;

static void runtime_libc_init(bool network_enabled) {
  libc_socket_config_t *config = NULL;
  if (network_enabled) {
    beam_socket_config = socket_config;
    beam_socket_config.tcp_socket_readable = NULL;
    beam_socket_config.tcp_socket_writable = NULL;
    beam_socket_config.tcp_socket_hup = NULL;
    config = &beam_socket_config;
  }
  libc_init(config, (void *)beam_heap_start, BEAM_HEAP_SIZE);
}

runtime_status_t runtime_lifecycle_start(void) {
  /* Configuration is the only pre-libc stage. No address from a patched blob
   * may be followed before this whole validation pass succeeds. */
  bool network_enabled = false;
  runtime_status_t status = runtime_config_validate(&network_enabled);
  if (status != RUNTIME_STATUS_OK) {
    return status;
  }

  runtime_serial_init();
  beam_boot_banner();

  /* Binding the filesystem precedes libc because libc_init_file installs its
   * syscall path from these globals. Active filesystem work needs cothreads
   * and therefore happens only after thread_init below. */
  runtime_fs_bind();
  runtime_libc_init(network_enabled);
  runtime_syscalls_register();
  rng_init();

  status = thread_init();
  if (status != RUNTIME_STATUS_OK) {
    return status;
  }

  status = runtime_fs_start(beam_warm_start());
  if (status != RUNTIME_STATUS_OK) {
    return status;
  }

  printf("Chrysopolis: beam_server up on the LionsOS reference stack.\n");
  struct timespec now = {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return RUNTIME_STATUS_CLOCK;
  }
  printf("monotonic clock via sDDF timer: %lld.%09lld s\n",
         (long long)now.tv_sec, (long long)now.tv_nsec);

  /* lwIP comes last among services. Its callbacks need libc and its periodic
   * work needs both the timer and the Microkit event lifecycle. */
  runtime_network_init(network_enabled);
  status = runtime_payload_start(network_enabled);
  if (status != RUNTIME_STATUS_OK) {
    return status;
  }

  printf("Chrysopolis: init() returned; Microkit event loop live.\n");
  return RUNTIME_STATUS_OK;
}

void runtime_lifecycle_notified(microkit_channel ch) {
  runtime_timer_notified(ch);
  runtime_network_notified(ch);
  thread_notified(ch);
  thread_io_wake();
  runtime_network_flush();
  runtime_payload_external_event(ch);
  microkit_cothread_yield();
}

static bool status_is_config_failure(runtime_status_t status) {
  return status >= RUNTIME_STATUS_CONFIG_SERIAL &&
         status <= RUNTIME_STATUS_CONFIG_NET_PAIR;
}

static const char *status_stage(runtime_status_t status) {
  if (status_is_config_failure(status)) {
    return "config";
  }
  switch (status) {
  case RUNTIME_STATUS_CONFIG_SERIAL:
  case RUNTIME_STATUS_CONFIG_TIMER:
  case RUNTIME_STATUS_CONFIG_FS:
  case RUNTIME_STATUS_CONFIG_NET:
  case RUNTIME_STATUS_CONFIG_LWIP:
  case RUNTIME_STATUS_CONFIG_NET_PAIR:
    return "config";
  case RUNTIME_STATUS_COTHREAD_ALLOC:
    return "cothread";
  case RUNTIME_STATUS_FS_COMMAND:
  case RUNTIME_STATUS_FS_MOUNT:
    return "filesystem";
  case RUNTIME_STATUS_CLOCK:
    return "timer";
  case RUNTIME_STATUS_ENVIRONMENT:
  case RUNTIME_STATUS_PAYLOAD_SPAWN:
    return "erts";
  case RUNTIME_STATUS_PROBE_SPAWN:
  case RUNTIME_STATUS_THREAD_PROBE:
    return "probe";
  case RUNTIME_STATUS_OK:
    return "none";
  }
  return "unknown";
}

static const char *status_name(runtime_status_t status) {
  switch (status) {
  case RUNTIME_STATUS_OK:
    return "ok";
  case RUNTIME_STATUS_CONFIG_SERIAL:
    return "config-serial";
  case RUNTIME_STATUS_CONFIG_TIMER:
    return "config-timer";
  case RUNTIME_STATUS_CONFIG_FS:
    return "config-fs";
  case RUNTIME_STATUS_CONFIG_NET:
    return "config-net";
  case RUNTIME_STATUS_CONFIG_LWIP:
    return "config-lwip";
  case RUNTIME_STATUS_CONFIG_NET_PAIR:
    return "config-net-pair";
  case RUNTIME_STATUS_COTHREAD_ALLOC:
    return "cothread-alloc";
  case RUNTIME_STATUS_FS_COMMAND:
    return "fs-command";
  case RUNTIME_STATUS_FS_MOUNT:
    return "fs-mount";
  case RUNTIME_STATUS_CLOCK:
    return "clock";
  case RUNTIME_STATUS_ENVIRONMENT:
    return "environment";
  case RUNTIME_STATUS_PAYLOAD_SPAWN:
    return "payload-spawn";
  case RUNTIME_STATUS_PROBE_SPAWN:
    return "probe-spawn";
  case RUNTIME_STATUS_THREAD_PROBE:
    return "thread-probe";
  }
  return "unknown";
}

static void debug_put_status(runtime_status_t status) {
  char reversed[4] = {};
  unsigned value = (unsigned)status;
  size_t length = 0;
  do {
    reversed[length++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);

  char output[4] = {};
  for (size_t i = 0; i < length; i++) {
    output[i] = reversed[length - i - 1];
  }
  microkit_dbg_puts(output);
}

void runtime_lifecycle_fail(runtime_status_t status) {
  microkit_dbg_puts("BEAM|lifecycle|FATAL|stage=");
  microkit_dbg_puts(status_stage(status));
  microkit_dbg_puts("|status=");
  microkit_dbg_puts(status_name(status));
  microkit_dbg_puts("|code=");
  debug_put_status(status);
  microkit_dbg_puts("\n");

  if (!status_is_config_failure(status)) {
    beam_request_restart((int)status);
  }

  for (;;) {
    seL4_Yield();
  }
}
