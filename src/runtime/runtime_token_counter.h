#ifndef CHRYSOPOLIS_RUNTIME_TOKEN_COUNTER_H
#define CHRYSOPOLIS_RUNTIME_TOKEN_COUNTER_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * A monotonically increasing identifier source that never reuses a value.
 * Pure: the caller owns the counter and any locking.
 */
typedef struct {
  /* The next value to hand out. UINTPTR_MAX is never handed out. */
  uintptr_t next;
} runtime_token_counter;

/* Store the next identifier in *token and advance. Returns false without
 * writing once the space is exhausted. counter and token must be non-null. */
[[__nodiscard__]] static inline bool
runtime_token_counter_next(runtime_token_counter *counter, uintptr_t *token) {
  if (counter->next == UINTPTR_MAX) {
    return false;
  }
  *token = counter->next;
  counter->next++;
  return true;
}

#endif
