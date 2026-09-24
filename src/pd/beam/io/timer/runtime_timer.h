#ifndef CHRYSOPOLIS_RUNTIME_TIMER_H
#define CHRYSOPOLIS_RUNTIME_TIMER_H 1

#include <stdint.h>

#include <microkit.h>

/*
 * Arm the PD's single sDDF timer slot for an absolute CLOCK_MONOTONIC deadline
 * in nanoseconds. The timer configuration must already be valid. An expired
 * deadline is made immediately pending, and a later deadline never displaces
 * an earlier armed one. This function neither blocks nor reports failure.
 */
void beam_timer_arm(uint64_t deadline_ns);

/* Clear the timer-multiplexer slot when its notification arrives. */
void runtime_timer_notified(microkit_channel ch);

#endif
