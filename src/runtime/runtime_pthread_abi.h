#ifndef CHRYSOPOLIS_RUNTIME_PTHREAD_ABI_H
#define CHRYSOPOLIS_RUNTIME_PTHREAD_ABI_H 1

#include <pthread.h>
#include <stddef.h>

/* Representation dependency on the pinned LionsOS musl pthread.h and
 * src/internal/pthread_impl.h. Only ABI adapters include this header. The
 * ordinary int fields below use direct __atomic operations, not _Atomic casts.
 * Revalidate these sizes and indices when the pinned musl revision changes. */
enum { RUNTIME_ATTR_DETACH_INDEX = 6, RUNTIME_ATTR_STACK_WORD = 0 };
enum {
  RUNTIME_MUTEX_OWNER_WORD = 0,
  RUNTIME_MUTEX_DEPTH_WORD = 1,
  RUNTIME_MUTEX_TYPE_WORD = 3,
  RUNTIME_RW_STATE_WORD = 0,
  RUNTIME_RW_WRITER_WORD = 1,
  RUNTIME_COND_GENERATION_WORD = 0,
};
/* The freestanding compiler's limits.h omits the musl minimum stack size. */
enum { RUNTIME_MIN_STACK_SIZE = 2048 };

static_assert(sizeof(long) == 8);
static_assert(sizeof(pthread_t) == sizeof(void *));
static_assert(sizeof(pthread_attr_t) == 56);
static_assert(sizeof(pthread_mutex_t) == 40);
static_assert(sizeof(pthread_rwlock_t) == 56);
static_assert(sizeof(pthread_cond_t) == 48);
static_assert(sizeof(pthread_once_t) == sizeof(int));
static_assert(sizeof(pthread_spinlock_t) == sizeof(int));
static_assert(RUNTIME_ATTR_DETACH_INDEX <
              sizeof(((pthread_attr_t *)0)->__u.__i) / sizeof(int));
static_assert(RUNTIME_ATTR_STACK_WORD <
              sizeof(((pthread_attr_t *)0)->__u.__s) / sizeof(size_t));
static_assert(RUNTIME_MUTEX_TYPE_WORD <
              sizeof(((pthread_mutex_t *)0)->__u.__i) / sizeof(int));
static_assert(RUNTIME_RW_WRITER_WORD <
              sizeof(((pthread_rwlock_t *)0)->__u.__i) / sizeof(int));
static_assert(RUNTIME_COND_GENERATION_WORD <
              sizeof(((pthread_cond_t *)0)->__u.__i) / sizeof(int));

/* ERTS requires these legacy ABI entries. This musl pthread.h does not
 * declare them, so the adapter supplies prototypes for its own definitions. */
int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr);
int pthread_sigmask(int how, const void *set, void *oldset);

#endif
