#ifndef CHRYSOPOLIS_RUNTIME_BOOT_H
#define CHRYSOPOLIS_RUNTIME_BOOT_H 1

#include <microkit.h>

/*
 * Optional ERTS entry points. A missing weak symbol compares equal to NULL.
 * Callers must test it before calling; neither function takes ownership of
 * its arguments. erl_start() is entered only after the runtime, libc and
 * cothread scheduler are ready.
 */
extern void erl_start(int argc, char **argv) __attribute__((weak));
extern void beam_process_external_events(microkit_channel ch)
    __attribute__((weak));

/*
 * Install the syscall compatibility layer after libc_init() has populated its
 * syscall table and before RNG initialisation or ERTS startup. This has no
 * recoverable failure result and must run once per cold or restored boot.
 */
void bringup_register_syscalls(void);

#endif
