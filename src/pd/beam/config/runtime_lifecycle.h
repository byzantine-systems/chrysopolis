#ifndef CHRYSOPOLIS_RUNTIME_LIFECYCLE_H
#define CHRYSOPOLIS_RUNTIME_LIFECYCLE_H 1

#include "runtime_status.h"

#include <microkit.h>

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
