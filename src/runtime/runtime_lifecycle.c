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

void runtime_lifecycle_fail(runtime_status_t status) {
  microkit_dbg_puts("BEAM|lifecycle|FATAL|stage=");
  microkit_dbg_puts(runtime_status_stage(status));
  microkit_dbg_puts("|status=");
  microkit_dbg_puts(runtime_status_name(status));
  microkit_dbg_puts("|code=");
  char code[runtime_status_code_text_size] = {};
  (void)runtime_status_format_code(status, code);
  microkit_dbg_puts(code);
  microkit_dbg_puts("\n");

  if (!runtime_status_is_config_failure(status)) {
    beam_request_restart((int)status);
  }

  for (;;) {
    seL4_Yield();
  }
}
