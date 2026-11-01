#ifndef CHRYSOPOLIS_HOST_PROP_H
#define CHRYSOPOLIS_HOST_PROP_H 1

/*
 * Seeded generation and sequence shrinking for the host property suites.
 *
 * A property runs a command sequence and returns true when it holds. On a
 * failure prop_shrink removes chunks of commands (ddmin) while the property
 * still fails, so the reported counterexample cannot lose any one chunk and
 * still fail. Seeds are fixed per suite, so every failure replays exactly;
 * the suite prints the seed and the shrunk sequence.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* xorshift64 state. Build one with prop_seed, which keeps it nonzero. */
typedef struct {
  uint64_t state;
} prop_rng;

[[nodiscard]] prop_rng prop_seed(uint64_t seed);

/* The next 64-bit value. rng must be non-null. */
uint64_t prop_next(prop_rng *rng);

/* A value in [0, bound), or 0 when bound is 0. The modulo bias does not
 * matter for test generation. */
size_t prop_below(prop_rng *rng, size_t bound);

/* True with probability percent/100. */
bool prop_chance(prop_rng *rng, unsigned int percent);

/* Returns true when the property holds for items[0..count). */
typedef bool prop_holds_fn(const void *items, size_t count, void *ctx);

/*
 * Shrinks items[0..count), each element size bytes, in place while holds()
 * stays false, and returns the new count. scratch must hold count * size
 * bytes and must not overlap items. holds() runs on scratch, never on items.
 */
size_t prop_shrink(void *items, size_t count, size_t size, void *scratch,
                   prop_holds_fn *holds, void *ctx);

#endif
