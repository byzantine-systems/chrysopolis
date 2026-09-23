/*
 * Identity, resource limits and process exit.
 *
 * ERTS queries these during boot to size its tables and name its host. The
 * answers are fixed: a root user, one online CPU to match +S 1:1, a 1024
 * descriptor limit and a Linux-shaped uname. libc's posix.c answers getuid
 * and getgid itself.
 *
 * exit and exit_group: musl's _Exit calls exit_group and then loops on exit,
 * and libc implements neither, so an ERTS abort would spin forever flooding
 * the console. A Microkit PD cannot exit, so the exit status goes to Root as a
 * restart request (restart.c explains how the code reaches Root). Root resets
 * this PD's memory and re-enters it at a clean boot, and the shell comes back
 * at a fresh 1>. An init:stop() or an ERTS abort therefore recovers instead of
 * wedging the PD.
 */
#include "runtime_fd.h"
#include "runtime_restart.h"
#include "runtime_syscall_handlers.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/utsname.h>

/* geteuid and getegid. */
long runtime_sys_getid_root(va_list ap) {
  (void)ap;
  return 0;
}

/* ERTS sizes its scheduler pool from the affinity mask. A null mask or one
 * shorter than a word is left untouched and the call still succeeds. */
long runtime_sys_sched_getaffinity(va_list ap) {
  (void)va_arg(ap, long); /* pid */
  const size_t cpusetsize = runtime_sys_arg_size(va_arg(ap, long));
  unsigned long *mask = runtime_sys_arg_pointer(va_arg(ap, long));
  if (mask != nullptr && cpusetsize >= sizeof(unsigned long)) {
    memset(mask, 0, cpusetsize);
    mask[0] = 1;
  }
  return (long)sizeof(unsigned long);
}

long runtime_sys_uname(va_list ap) {
  struct utsname *u = runtime_sys_arg_pointer(va_arg(ap, long));
  if (u != nullptr) {
    *u = (struct utsname){};
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "chrysopolis");
    strcpy(u->release, "6.0.0-sel4");
    strcpy(u->version, "Chrysopolis LionsOS");
    strcpy(u->machine, "aarch64");
  }
  return 0;
}

/* erl_prim_loader needs a current directory. The syscall fills the buffer and
 * returns the length including the NUL terminator. */
long runtime_sys_getcwd(va_list ap) {
  char *buf = runtime_sys_arg_pointer(va_arg(ap, long));
  const size_t size = runtime_sys_arg_size(va_arg(ap, long));
  if (buf == nullptr) {
    return -EFAULT;
  }
  if (size < 2) {
    return -ERANGE;
  }
  buf[0] = '/';
  buf[1] = '\0';
  return 2;
}

/* ERTS sizes its descriptor tables from RLIMIT_NOFILE. Every resource reports
 * the same limit. */
long runtime_sys_getrlimit(va_list ap) {
  (void)va_arg(ap, long); /* resource */
  struct rlimit *rl = runtime_sys_arg_pointer(va_arg(ap, long));
  if (rl != nullptr) {
    rl->rlim_cur = runtime_fd_limit;
    rl->rlim_max = runtime_fd_limit;
  }
  return 0;
}

/* New limits are ignored. */
long runtime_sys_prlimit64(va_list ap) {
  (void)va_arg(ap, long); /* pid */
  (void)va_arg(ap, long); /* resource */
  (void)va_arg(ap, long); /* new_limit */
  struct rlimit *old = runtime_sys_arg_pointer(va_arg(ap, long));
  if (old != nullptr) {
    old->rlim_cur = runtime_fd_limit;
    old->rlim_max = runtime_fd_limit;
  }
  return 0;
}

long runtime_sys_exit(va_list ap) {
  const int status = runtime_sys_arg_int(va_arg(ap, long));
  beam_request_restart(status);
}
