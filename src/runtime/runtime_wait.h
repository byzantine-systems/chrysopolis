#ifndef CHRYSOPOLIS_RUNTIME_WAIT_H
#define CHRYSOPOLIS_RUNTIME_WAIT_H 1

/*
 * Park the current cothread until the shared event generation advances.
 * Wakeups are broadcast and may be unrelated to the caller, so every caller
 * must re-check its own predicate in a loop. On the root thread this yields
 * once instead of blocking. There is no failure result.
 */
void thread_io_wait(void);

/*
 * Advance the event generation and wake all cothreads already parked in
 * thread_io_wait(). Safe from the root thread or a running cothread. A wake
 * with no waiter is harmless and may leave a latched semaphore signal.
 */
void thread_io_wake(void);

/*
 * Park the current execution context permanently without consuming CPU. This
 * function does not return and provides no cleanup path.
 */
[[__noreturn__]] void thread_park_forever(void);

#endif
