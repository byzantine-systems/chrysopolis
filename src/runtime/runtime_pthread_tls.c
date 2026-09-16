/*
 * pthread keys for cooperative cothreads.
 *
 * musl's single main-thread errno remains safe because cothreads only switch
 * at explicit blocking points. Key values instead belong to the running
 * libmicrokitco slot and must be cleared before that slot can be recycled.
 */
#include "runtime_pthread_tls.h"
#include "runtime_cothread_state.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

enum { RUNTIME_MAX_KEYS = 64 };

typedef struct {
  void (*destructor)(void *);
  bool used;
} runtime_key_slot;

static runtime_key_slot key_table[RUNTIME_MAX_KEYS];
static void *co_tsd[LIBMICROKITCO_MAX_COTHREADS][RUNTIME_MAX_KEYS];

static bool key_valid(pthread_key_t key) {
  return key < RUNTIME_MAX_KEYS && key_table[key].used;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  if (key == NULL) {
    return EINVAL;
  }
  for (size_t i = 0; i < RUNTIME_MAX_KEYS; i++) {
    if (!key_table[i].used) {
      for (size_t h = 0; h < LIBMICROKITCO_MAX_COTHREADS; h++) {
        co_tsd[h][i] = NULL;
      }
      key_table[i].destructor = destructor;
      key_table[i].used = true;
      *key = (pthread_key_t)i;
      return 0;
    }
  }
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (!key_valid(key)) {
    return EINVAL;
  }
  key_table[key] = (runtime_key_slot){};
  for (size_t h = 0; h < LIBMICROKITCO_MAX_COTHREADS; h++) {
    co_tsd[h][key] = NULL;
  }
  return 0;
}

void *pthread_getspecific(pthread_key_t key) {
  if (!key_valid(key)) {
    return NULL;
  }
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_handle_valid(handle)) {
    return NULL;
  }
  return co_tsd[(size_t)handle][key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (!key_valid(key)) {
    return EINVAL;
  }
  microkit_cothread_ref_t handle = runtime_co_current();
  if (!runtime_co_handle_valid(handle)) {
    return EINVAL;
  }
  co_tsd[(size_t)handle][key] = (void *)value;
  return 0;
}

void runtime_tls_clear(microkit_cothread_ref_t handle) {
  if (!runtime_co_handle_valid(handle)) {
    return;
  }
  for (size_t key = 0; key < RUNTIME_MAX_KEYS; key++) {
    co_tsd[(size_t)handle][key] = NULL;
  }
}

void runtime_tls_finish(microkit_cothread_ref_t handle) {
  if (!runtime_co_handle_valid(handle)) {
    return;
  }
  for (size_t pass = 0; pass < RUNTIME_TSD_DESTRUCTOR_PASSES; pass++) {
    bool called = false;
    for (size_t key = 0; key < RUNTIME_MAX_KEYS; key++) {
      void *value = co_tsd[(size_t)handle][key];
      void (*destructor)(void *) = key_table[key].destructor;
      if (key_table[key].used && value != NULL && destructor != NULL) {
        co_tsd[(size_t)handle][key] = NULL;
        destructor(value);
        called = true;
      }
    }
    if (!called) {
      break;
    }
  }
  runtime_tls_clear(handle);
}
