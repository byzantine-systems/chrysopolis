/*
 * The payload is compared by length and bytes, never as a C string: a writer
 * owes no terminator, and a NUL inside the payload must not end a match early.
 */
#include "runtime_pd_restart_parse.h"

#include <string.h>

static bool is_trailing_space(char c) {
  return c == '\n' || c == '\r' || c == ' ' || c == '\t';
}

static constexpr char fault_prefix[] = "fault:";
static constexpr size_t fault_prefix_len = sizeof(fault_prefix) - 1;

runtime_pd_restart_parse_status
runtime_pd_restart_parse(const char *buf, size_t count,
                         const char *const names[static 1], size_t class_count,
                         size_t *mode, size_t *class_index) {
  while (count > 0 && is_trailing_space(buf[count - 1])) {
    count--;
  }

  size_t request_mode = runtime_pd_restart_mode_healthy;
  /* A bare "fault:" names no class, so the prefix needs at least one more
   * byte to count. */
  if (count > fault_prefix_len &&
      memcmp(buf, fault_prefix, fault_prefix_len) == 0) {
    request_mode = runtime_pd_restart_mode_fault;
    buf += fault_prefix_len;
    count -= fault_prefix_len;
  }

  for (size_t i = 0; i < class_count; i++) {
    const size_t len = strlen(names[i]);
    if (count == len && len > 0 && memcmp(buf, names[i], len) == 0) {
      *mode = request_mode;
      *class_index = i;
      return runtime_pd_restart_parse_ok;
    }
  }
  return runtime_pd_restart_parse_unknown;
}

bool runtime_pd_restart_any_channel(
    size_t modes, size_t classes,
    const volatile uint8_t channels[static modes][classes], uint8_t none) {
  for (size_t mode = 0; mode < modes; mode++) {
    for (size_t class_index = 0; class_index < classes; class_index++) {
      if (channels[mode][class_index] != none) {
        return true;
      }
    }
  }
  return false;
}
