#ifndef CHRYSOPOLIS_RUNTIME_CONTROL_DEV_H
#define CHRYSOPOLIS_RUNTIME_CONTROL_DEV_H 1

#include <stdbool.h>

/* True for /dev/chrysostat and /dev/chrysoctl, absolute or cwd-relative.
 * path must be non-null and is not retained. */
[[__nodiscard__]] bool runtime_control_dev_handles_path(const char *path);

/* Open a path runtime_control_dev_handles_path accepted, with the caller's
 * open(2) flags. Returns the descriptor or a negative errno. */
[[__nodiscard__]] int runtime_control_dev_open(const char *path, int flags);

#endif
