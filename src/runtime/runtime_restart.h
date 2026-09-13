#ifndef CHRYSOPOLIS_RUNTIME_RESTART_H
#define CHRYSOPOLIS_RUNTIME_RESTART_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * Base of the persistent snapshot region patched by the Microkit image tool.
 * It has static lifetime, is valid before libc, and is never owned or freed by
 * a caller.
 */
extern uintptr_t beam_snapshot_start;

/*
 * Pre-libc entry helpers called from the assembly boot/reset trampolines.
 * beam_boot_capture() receives the incoming stack pointer and may use only
 * dependency-free Microkit operations. beam_reset_restore() runs on the
 * private reset stack, restores writable memory, and returns the cold-boot
 * stack pointer. Neither operation may allocate, yield, or retain arguments.
 */
void beam_boot_capture(uintptr_t sp);
[[__nodiscard__]] uintptr_t beam_reset_restore(void);

/* Query and report restart state after normal runtime initialisation begins. */
[[__nodiscard__]] bool beam_warm_start(void);
void beam_boot_banner(void);

/*
 * Report status to Root by deliberately faulting. Only the low eight bits are
 * carried in the fault address. This function never returns or performs
 * caller-visible cleanup.
 */
[[__noreturn__]] void beam_request_restart(int status);

#endif
