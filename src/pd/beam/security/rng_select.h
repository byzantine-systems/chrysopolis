#ifndef CHRYSOPOLIS_RNG_SELECT_H
#define CHRYSOPOLIS_RNG_SELECT_H 1

#include "rng.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Entropy source selection and the jitter sample accumulator behind rng.c.
 * Pure: counter reads, the DRBG and logging stay in rng.c. Nothing here may
 * log or retain seed material; callers must not print accumulator state.
 */

/* A seed shorter than this sends rng_init to the documented fallback. */
static constexpr size_t rng_seed_min_bytes = 32;

typedef enum {
  rng_seed_source_jitter,
  rng_seed_source_fallback,
} rng_seed_source;

/* Choose the seed source from the jitter provider's output length. */
[[__nodiscard__]] static inline rng_seed_source
rng_select_seed(size_t jitter_bytes) {
  return jitter_bytes >= rng_seed_min_bytes ? rng_seed_source_jitter
                                            : rng_seed_source_fallback;
}

/* The boot-line name of a seed source. Never null. */
[[__nodiscard__]] static inline const char *
rng_seed_source_name(rng_seed_source source) {
  return source == rng_seed_source_jitter ? "jitter" : "fallback";
}

/* True when a reseed round should call the provider: the DRBG is
 * instantiated and the provider and its collect callback exist. */
[[__nodiscard__]] static inline bool
rng_reseed_applies(bool drbg_ready, const rng_provider_t *provider) {
  return drbg_ready && provider != nullptr && provider->collect != nullptr;
}

/* Seconds added to CLOCK_REALTIME for a boot: raw reduced below 15 minutes,
 * enough to make time-seeded refs differ across boots while keeping the wall
 * clock near the base epoch. */
static constexpr uint32_t rng_realtime_offset_modulo = 900;

[[__nodiscard__]] static inline uint32_t rng_realtime_offset(uint32_t raw) {
  return raw % rng_realtime_offset_modulo;
}

enum : size_t { rng_jitter_lanes = 8 };

/* Health floor: at least this many sample deltas must differ from the delta
 * before them. A lockstep counter (QEMU -icount) yields near-constant deltas
 * and fails it. */
static constexpr unsigned rng_jitter_min_distinct = 64;

/* The seed pool, 8 lanes of 64 bits, and the running health count. */
typedef struct {
  uint64_t pool[rng_jitter_lanes];
  uint64_t last_delta;
  unsigned distinct;
} rng_jitter_state;

/* Start a collection with the first counter reading folded into lane 0. */
void rng_jitter_init(rng_jitter_state *state, uint64_t first_counter);

/* The byte the collection loop stores into its scratch buffer before taking
 * sample number sample, so successive counter reads bracket real work. */
[[__nodiscard__]] uint8_t rng_jitter_scratch_byte(const rng_jitter_state *state,
                                                  size_t sample);

/* Fold sample number sample, the counter delta since the previous read, into
 * its lane and update the distinct-delta count. */
void rng_jitter_absorb(rng_jitter_state *state, size_t sample, uint64_t delta);

/* True when enough deltas differed to trust the pool. */
[[__nodiscard__]] bool rng_jitter_healthy(const rng_jitter_state *state);

/*
 * End a collection. When healthy, copy up to len raw pool bytes (at most the
 * pool size) into buf and return the count; otherwise return 0 without
 * writing buf. Either way the whole state is wiped. buf may be null only when
 * len is 0.
 */
[[__nodiscard__]] size_t rng_jitter_finish(rng_jitter_state *state,
                                           uint8_t *buf, size_t len);

#endif
