/*
 * Microkit lifecycle entry points for beam_server.
 *
 * Initialization and event ordering live in runtime_lifecycle.c. Keeping this
 * file limited to Microkit's callback surface makes it clear that init either
 * completes the entire lifecycle or terminates without exposing partial state.
 */
#include "runtime_lifecycle.h"

void init(void) {
  const runtime_status_t status = runtime_lifecycle_start();
  if (status != RUNTIME_STATUS_OK) {
    runtime_lifecycle_fail(status);
  }
}

void notified(microkit_channel ch) { runtime_lifecycle_notified(ch); }
