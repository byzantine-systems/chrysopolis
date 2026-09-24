#ifndef CHRYSOPOLIS_RUNTIME_PTHREAD_HANDLE_H
#define CHRYSOPOLIS_RUNTIME_PTHREAD_HANDLE_H 1

#include <pthread.h>
#include <stdbool.h>

/* Logical pthread IDs are never recycled within a boot, even when a
 * libmicrokitco slot is reused. A token is an opaque value, never dereferenced.
 * These functions run on the single TCB and require no external locking. */
[[nodiscard]] bool runtime_thread_token_next(pthread_t *token);
/* A null output or exhausted token space returns false without writing. */
[[nodiscard]] pthread_t runtime_thread_root_token(void);
[[nodiscard]] bool runtime_thread_token_is_root(pthread_t token);

#endif
