/*
 * Conversion between the pinned musl pthread_t pointer ABI and logical IDs.
 *
 * No pthread_t is dereferenced. AArch64's uintptr_t can carry a pointer-sized
 * token, as the former handle+1 encoding already relied on. IDs are never
 * reused within a boot, even when libmicrokitco recycles a cothread slot.
 */
#include "runtime_pthread_handle.h"

#include <libmicrokitco.h>

#include <stdint.h>

/* Reserve 1 for root and 2..MAX for cothreads spawned outside pthread_create.
 */
static uintptr_t next_token = LIBMICROKITCO_MAX_COTHREADS + 1;
static_assert(sizeof(pthread_t) == sizeof(uintptr_t));

pthread_t runtime_thread_root_token(void) { return (pthread_t)(uintptr_t)1; }

bool runtime_thread_token_is_root(pthread_t token) {
  return (uintptr_t)token == 1;
}

bool runtime_thread_token_next(pthread_t *token) {
  if (token == NULL || next_token == UINTPTR_MAX) {
    return false;
  }
  *token = (pthread_t)next_token;
  next_token++;
  return true;
}
