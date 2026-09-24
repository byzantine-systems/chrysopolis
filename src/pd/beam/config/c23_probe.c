#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "Chrysopolis first-party runtime requires C23"
#endif

#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__ != 0
#error "Chrysopolis runtime must use a freestanding C implementation"
#endif

static_assert(sizeof(uintptr_t) == sizeof(void *));
static_assert(CHAR_BIT == 8);
static_assert(sizeof(uintptr_t) == 8);
static_assert(ATOMIC_BOOL_LOCK_FREE >= 0 && ATOMIC_BOOL_LOCK_FREE <= 2);
