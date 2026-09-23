#ifndef CHRYSOPOLIS_RUNTIME_THREAD_PROBE_H
#define CHRYSOPOLIS_RUNTIME_THREAD_PROBE_H 1

#include <stdbool.h>

/* Diagnostic-image-only C probe. Runs on the root context after thread_init
 * and before ERTS or the network bring-up probe starts. */
[[nodiscard]] bool runtime_thread_probe_run(void);

#endif
