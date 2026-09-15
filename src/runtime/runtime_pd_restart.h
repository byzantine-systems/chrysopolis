#ifndef CHRYSOPOLIS_RUNTIME_PD_RESTART_H
#define CHRYSOPOLIS_RUNTIME_PD_RESTART_H 1

#include <stdbool.h>

/*
 * True when path names the test-only driver-restart trigger AND this image
 * wired at least one restart channel. A production image returns false for
 * every path, so the caller falls through to the real filesystem exactly as
 * if the trigger did not exist. path must be non-NULL and is not retained.
 */
[[__nodiscard__]] bool runtime_pd_restart_handles_path(const char *path);

/* Open a write-only descriptor for the trigger. Returns it or -EMFILE. */
[[__nodiscard__]] int runtime_pd_restart_open(void);

#endif
