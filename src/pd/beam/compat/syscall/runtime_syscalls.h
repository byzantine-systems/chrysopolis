#ifndef CHRYSOPOLIS_RUNTIME_SYSCALLS_H
#define CHRYSOPOLIS_RUNTIME_SYSCALLS_H 1

/*
 * Install the syscall compatibility layer after libc_init() has populated its
 * syscall table and before RNG initialisation or ERTS startup. This has no
 * recoverable failure result and must run once per cold or restored boot:
 * libc_define_syscall asserts that every slot it fills is still empty.
 */
void runtime_syscalls_register(void);

#endif
