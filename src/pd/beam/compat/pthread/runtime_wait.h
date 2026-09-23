#ifndef CHRYSOPOLIS_RUNTIME_WAIT_H
#define CHRYSOPOLIS_RUNTIME_WAIT_H 1

/* Boot-only initialization of private wait semaphores. thread_init calls this
 * once before publishing scheduler readiness; no waiter may exist yet. */
void runtime_wait_init(void);

/*
 * Park the current cothread until the shared event generation advances.
 * Wakeups are broadcast and may be unrelated to the caller, so every caller
 * must re-check its own predicate in a loop. On the root thread this yields
 * once instead of blocking. There is no failure result or intrinsic timeout:
 * timed callers arm the sDDF timer and recheck their deadline after a wake.
 */
void thread_io_wait(void);

/*
 * Advance the event generation and wake all cothreads already parked in
 * thread_io_wait(). Safe from the root thread or a running cothread. A wake
 * with no waiter is harmless and may leave a latched semaphore signal.
 */
void thread_io_wake(void);

/*
 * Park the current worker permanently. On the root context the fallback
 * yields to seL4 repeatedly, so callers must not expect Microkit's event
 * loop to resume from that path. This is an intentional terminal state for a
 * thread with no incoming data; it does not return or run cleanup.
 */
[[__noreturn__]] void thread_park_forever(void);

#endif
