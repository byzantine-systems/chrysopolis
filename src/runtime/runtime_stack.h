#ifndef CHRYSOPOLIS_RUNTIME_STACK_H
#define CHRYSOPOLIS_RUNTIME_STACK_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Cothread stack acquisition with all-or-nothing ownership. Pure: no globals,
 * no libc allocation of its own, safe on the host test harness.
 */

/* Smallest stack a caller may request. */
static constexpr size_t runtime_stack_min_bytes = 4096;

typedef void *(*runtime_stack_alloc_fn)(size_t bytes, void *context);
typedef void (*runtime_stack_free_fn)(void *pointer, void *context);

/*
 * Fill stacks[0..count) with count distinct allocations of at least bytes.
 * allocate must return a distinct allocation or nullptr; release accepts every
 * pointer allocate returned. Both run synchronously and context is never
 * retained.
 *
 * Invalid arguments (null stacks or callbacks, count zero, bytes below
 * runtime_stack_min_bytes) return false without calling either callback and
 * without writing stacks. On allocation failure, or an allocation whose end
 * would wrap the address space, every allocation made by this call is
 * released in reverse order, every slot is zero, and the result is false. On
 * success the caller owns every allocation.
 */
[[__nodiscard__]] bool runtime_stack_prepare_n(uintptr_t stacks[], size_t count,
                                               size_t bytes,
                                               runtime_stack_alloc_fn allocate,
                                               runtime_stack_free_fn release,
                                               void *context);

#endif
