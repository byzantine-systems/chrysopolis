/*
 * pthread attributes accepted by ERTS on the fixed-stack cothread runtime.
 * Each libmicrokitco worker has a 512 KiB stack; a smaller requested minimum
 * is met by that stack, while a larger one cannot be promised. There are no
 * per-cothread guard pages in the current LionsOS heap mapping.
 */
#include "runtime_cothread_state.h"
#include "runtime_pthread_abi.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>

int pthread_attr_init(pthread_attr_t *attr) {
  if (attr == NULL) {
    return EINVAL;
  }
  *attr = (pthread_attr_t){};
  attr->__u.__s[RUNTIME_ATTR_STACK_WORD] = RUNTIME_CO_STACK_SIZE;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
  return attr == NULL ? EINVAL : 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
  if (attr == NULL || stacksize < RUNTIME_MIN_STACK_SIZE ||
      stacksize > RUNTIME_CO_STACK_SIZE) {
    return EINVAL;
  }
  attr->__u.__s[RUNTIME_ATTR_STACK_WORD] = stacksize;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
  if (attr == NULL || stacksize == NULL) {
    return EINVAL;
  }
  size_t requested = attr->__u.__s[RUNTIME_ATTR_STACK_WORD];
  *stacksize = requested == 0 ? RUNTIME_CO_STACK_SIZE : requested;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
  if (attr == NULL || (detachstate != PTHREAD_CREATE_JOINABLE &&
                       detachstate != PTHREAD_CREATE_DETACHED)) {
    return EINVAL;
  }
  attr->__u.__i[RUNTIME_ATTR_DETACH_INDEX] = detachstate;
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
  if (attr == NULL || detachstate == NULL) {
    return EINVAL;
  }
  *detachstate = attr->__u.__i[RUNTIME_ATTR_DETACH_INDEX];
  return 0;
}

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr) {
  if (attr == NULL) {
    return EINVAL;
  }
  return stackaddr == NULL ? 0 : ENOTSUP;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize) {
  if (attr == NULL) {
    return EINVAL;
  }
  return guardsize == 0 ? 0 : ENOTSUP;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
  (void)policy;
  return attr == NULL ? EINVAL : 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
  (void)inheritsched;
  return attr == NULL ? EINVAL : 0;
}

int pthread_sigmask(int how, const void *set, void *oldset) {
  (void)how;
  (void)set;
  (void)oldset;
  return 0;
}
