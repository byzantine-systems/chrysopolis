#ifndef CHRYSOPOLIS_RUNTIME_CONTROL_DEV_H
#define CHRYSOPOLIS_RUNTIME_CONTROL_DEV_H 1

#include <stdbool.h>
#include <stdint.h>

/* True for /dev/chrysostat and /dev/chrysoctl, absolute or cwd-relative.
 * path must be non-null and is not retained. */
[[__nodiscard__]] bool runtime_control_dev_handles_path(const char *path);

/* Open a path runtime_control_dev_handles_path accepted, with the caller's
 * open(2) flags. Returns the descriptor or a negative errno. */
[[__nodiscard__]] int runtime_control_dev_open(const char *path, int flags);

/* The BEAM's own row on Root's page, for the boot selector before ERTS
 * starts. False when the page is unmapped, torn after retries or invalid,
 * and then the outputs are unchanged. Not reentrant with a /dev/chrysostat
 * read, which shares its buffers, so call it only before ERTS runs. */
[[__nodiscard__]] bool runtime_control_dev_beam_row(uint32_t *lifetime_count,
                                                    uint32_t *effective_budget);

#endif
