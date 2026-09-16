#ifndef CHRYSOPOLIS_RUNTIME_SYSCALL_HANDLERS_H
#define CHRYSOPOLIS_RUNTIME_SYSCALL_HANDLERS_H 1

#include <lions/posix/posix.h>

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Syscall handlers installed by runtime_syscalls_register(). Each one takes
 * the va_list sel4_vsyscall built from musl's register arguments and returns
 * a result or a negative errno, as the Linux syscall would. The libc syscall
 * table is their only caller. No handler retains a pointer argument after it
 * returns.
 *
 * musl's AArch64 __syscallN functions widen every argument to long before
 * calling LionsOS's variadic dispatcher. Each handler must therefore consume
 * every slot with va_arg(ap, long), then convert the value to the syscall's
 * declared argument type. Asking va_arg for int or a pointer when the caller
 * supplied long is undefined C, even though those reads happen to select the
 * expected register-save slot under the current AArch64 ABI.
 *
 * The pointer conversion below reverses musl's pointer-to-long conversion.
 * Chrysopolis supports only the pinned LP64 AArch64 target, so make that ABI
 * dependency a compile-time contract rather than an implicit assumption.
 */

static_assert(sizeof(int) * CHAR_BIT == 32);
static_assert(sizeof(long) * CHAR_BIT == 64);
static_assert(sizeof(long) == sizeof(uintptr_t));
static_assert(sizeof(void *) == sizeof(uintptr_t));

[[__nodiscard__]] static inline int runtime_sys_arg_int(long arg) {
  /* musl widened an int supplied by the C wrapper, so this restores it. A raw
   * syscall value outside int's range follows the target ABI's narrowing rule.
   */
  return (int)arg;
}

[[__nodiscard__]] static inline size_t runtime_sys_arg_size(long arg) {
  return (size_t)(unsigned long)arg;
}

[[__nodiscard__]] static inline void *runtime_sys_arg_pointer(long arg) {
  return (void *)(uintptr_t)(unsigned long)arg;
}

/* Descriptor plumbing ERTS creates for itself (runtime_sys_fd.c). */
long runtime_sys_pipe2(va_list ap);
long runtime_sys_timerfd_create(va_list ap);
long runtime_sys_socketpair(va_list ap);

/* Readiness and idle waits (runtime_sys_poll.c). */
long runtime_sys_epoll_create1(va_list ap);
long runtime_sys_epoll_ctl(va_list ap);
long runtime_sys_epoll_pwait(va_list ap);
long runtime_sys_ppoll(va_list ap);
long runtime_sys_pselect6(va_list ap);

/* Clocks and sleeps (runtime_sys_time.c). */
long runtime_sys_clock_getres(va_list ap);
long runtime_sys_clock_nanosleep(va_list ap);
long runtime_sys_clock_gettime(va_list ap);

/* Cothread-aware futex (runtime_sys_sync.c). */
long runtime_sys_futex(va_list ap);

/* Identity, resource limits and process exit (runtime_sys_identity.c). */
long runtime_sys_getid_root(va_list ap);
long runtime_sys_sched_getaffinity(va_list ap);
long runtime_sys_uname(va_list ap);
long runtime_sys_getcwd(va_list ap);
long runtime_sys_getrlimit(va_list ap);
long runtime_sys_prlimit64(va_list ap);
long runtime_sys_exit(va_list ap);

/* Virtual devices and entropy (runtime_sys_devices.c). */
long runtime_sys_getrandom(va_list ap);
long runtime_sys_openat(va_list ap);

/*
 * Record the handler libc_redefine_syscall displaced so the replacement can
 * chain to it. Called once during registration, before the slot can be
 * reached. A null previous handler makes the chained path report -ENOSYS.
 */
void runtime_sys_clock_gettime_chain(muslcsys_syscall_t previous);
void runtime_sys_openat_chain(muslcsys_syscall_t previous);

#endif
