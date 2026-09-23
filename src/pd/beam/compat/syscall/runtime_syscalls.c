/*
 * Syscall table for the beam_server PD.
 *
 * The LionsOS libc registers only the syscalls its own components need. ERTS
 * reaches for more during boot, and an empty slot makes sel4_vsyscall return
 * -ENOSYS, which ERTS answers by spinning. This file is the one place that
 * binds syscall numbers to the compatibility handlers in the runtime_sys_*.c
 * units.
 *
 * File I/O is absent from the table: libc_init_file registers
 * openat/newfstatat/readlinkat/lseek/mkdirat/unlinkat against the FAT
 * fs_server. TCP/IP is absent too: gen_tcp drives the libc socket layer
 * (sock.c, tcp.c, lwIP), and the poll handlers only report its readiness.
 */
#include "runtime_syscalls.h"
#include "runtime_console.h"
#include "runtime_syscall_handlers.h"

#include <lions/posix/posix.h>

#include <stddef.h>
#include <sys/syscall.h>

/* AArch64 syscall numbers. Pinned so a header change cannot move them. */
static_assert(SYS_geteuid == 175);
static_assert(SYS_getegid == 177);

/* Accepted and ignored: signal masks, affinity changes, thread names, madvise
 * hints, timerfd arming and thread signals have no effect in this PD. */
static long runtime_sys_ok(va_list ap) {
  (void)ap;
  return 0;
}

struct syscall_binding {
  int nr;
  muslcsys_syscall_t handler;
};

/* Slots libc leaves empty. libc_define_syscall asserts each is still NULL. */
static const struct syscall_binding defined_syscalls[] = {
    {SYS_geteuid, runtime_sys_getid_root},
    {SYS_getegid, runtime_sys_getid_root},
    {SYS_sched_getaffinity, runtime_sys_sched_getaffinity},
    {SYS_sched_setaffinity, runtime_sys_ok},
    {SYS_uname, runtime_sys_uname},
    {SYS_getcwd, runtime_sys_getcwd},
    {SYS_clock_getres, runtime_sys_clock_getres},
    {SYS_clock_nanosleep, runtime_sys_clock_nanosleep},
    {SYS_sched_yield, runtime_sys_ok},
    {SYS_rt_sigaction, runtime_sys_ok},
    {SYS_rt_sigprocmask, runtime_sys_ok},
    {SYS_sigaltstack, runtime_sys_ok},
    {SYS_madvise, runtime_sys_ok},
    {SYS_getrlimit, runtime_sys_getrlimit},
    {SYS_prlimit64, runtime_sys_prlimit64},
    {SYS_epoll_create1, runtime_sys_epoll_create1},
    {SYS_epoll_ctl, runtime_sys_epoll_ctl},
    {SYS_epoll_pwait, runtime_sys_epoll_pwait},
    {SYS_pselect6, runtime_sys_pselect6},
    /* sock.c would claim ppoll, but runtime_lifecycle.c nulls its poll
     * callbacks so the slot stays free for this handler. sock.c's sys_ppoll
     * neither yields nor parks, and ERTS boot needs a cothread-blocking one. */
    {SYS_ppoll, runtime_sys_ppoll},
    {SYS_pipe2, runtime_sys_pipe2},
    {SYS_timerfd_create, runtime_sys_timerfd_create},
    {SYS_timerfd_settime, runtime_sys_ok},
    {SYS_timerfd_gettime, runtime_sys_ok},
    {SYS_tkill, runtime_sys_ok},
    {SYS_tgkill, runtime_sys_ok},
    {SYS_futex, runtime_sys_futex},
    {SYS_prctl, runtime_sys_ok},
    {SYS_socketpair, runtime_sys_socketpair},
    {SYS_exit, runtime_sys_exit},
    {SYS_exit_group, runtime_sys_exit},
};

struct syscall_replacement {
  int nr;
  muslcsys_syscall_t handler;
  /* Receives the displaced handler, or nullptr when nothing chains to it. */
  void (*chain)(muslcsys_syscall_t previous);
};

/* Slots libc_init or libc_init_file already claimed. The lifecycle seeds the
 * DRBG right after registration. */
static const struct syscall_replacement replaced_syscalls[] = {
    {SYS_clock_gettime, runtime_sys_clock_gettime,
     runtime_sys_clock_gettime_chain},
    {SYS_getrandom, runtime_sys_getrandom, nullptr},
    {SYS_openat, runtime_sys_openat, runtime_sys_openat_chain},
};

void runtime_syscalls_register(void) {
  for (size_t i = 0; i < sizeof defined_syscalls / sizeof defined_syscalls[0];
       i++) {
    libc_define_syscall(defined_syscalls[i].nr, defined_syscalls[i].handler);
  }

  for (size_t i = 0; i < sizeof replaced_syscalls / sizeof replaced_syscalls[0];
       i++) {
    const struct syscall_replacement *r = &replaced_syscalls[i];
    const muslcsys_syscall_t previous =
        libc_redefine_syscall(r->nr, r->handler);
    if (r->chain != nullptr) {
      r->chain(previous);
    }
  }

  runtime_console_attach_stdin();
}
