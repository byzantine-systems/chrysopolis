/* Exposes every generated checker by name to abi_differential.zig and
 * suite_abi_vectors.c. */
#include "abi_shim.h"

#include <chrysopolis/root_control.h>
#include <chrysopolis/worker_identity.h>
#include <chrysopolis/worker_protocol.h>
#include <chrysopolis/worker_status.h>

#include <string.h>

static bool name_is(const char *name, size_t name_len, const char *expected) {
  return name_len == strlen(expected) && memcmp(name, expected, name_len) == 0;
}

uint8_t abi_shim_check(const char *name, size_t name_len, const uint8_t *bytes,
                       size_t len, size_t bank) {
  if (name_is(name, name_len, "root_status_page")) {
    return chryso_check_root_status_page(bytes, len);
  }
  if (name_is(name, name_len, "spec_header")) {
    return chryso_check_spec_header(bytes, len);
  }
  if (name_is(name, name_len, "spec_bank")) {
    return chryso_check_spec_bank(bytes, len, bank);
  }
  if (name_is(name, name_len, "ctl_command")) {
    return chryso_check_ctl_command(bytes, len);
  }
  if (name_is(name, name_len, "ctl_reply")) {
    return chryso_check_ctl_reply(bytes, len);
  }
  if (name_is(name, name_len, "worker_identity")) {
    return chryso_check_worker_identity(bytes, len);
  }
  if (name_is(name, name_len, "worker_status")) {
    return chryso_check_worker_status(bytes, len);
  }
  if (name_is(name, name_len, "request_journal")) {
    return chryso_check_request_journal(bytes, len);
  }
  if (name_is(name, name_len, "completion_journal")) {
    return chryso_check_completion_journal(bytes, len);
  }
  return UINT8_MAX;
}
