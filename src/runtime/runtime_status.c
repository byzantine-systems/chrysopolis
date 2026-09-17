/*
 * Diagnostic text for boot statuses. The switches list every enumerator so
 * -Wswitch flags a status added without a name or stage.
 */
#include "runtime_status.h"

const char *runtime_status_stage(runtime_status_t status) {
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

const char *runtime_status_name(runtime_status_t status) {
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

size_t
runtime_status_format_code(runtime_status_t status,
                           char out[static runtime_status_code_text_size]) {
  unsigned value = runtime_status_exit_code(status);
  char reversed[runtime_status_code_text_size - 1] = {};
  size_t length = 0;
  do {
    reversed[length++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);

  for (size_t i = 0; i < length; i++) {
    out[i] = reversed[length - i - 1];
  }
  out[length] = '\0';
  return length;
}
