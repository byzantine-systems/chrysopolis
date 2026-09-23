/*
 * FNV-1a folding of timing samples into the seed pool. FNV is a cheap,
 * dependency-free accumulator that only packs the samples; HMAC-DRBG does the
 * real conditioning when rng.c hands it the pool.
 */
#include "rng_select.h"

#include <string.h>

static constexpr uint64_t fnv64_offset = 0xcbf29ce484222325u;
static constexpr uint64_t fnv64_prime = 0x100000001b3u;
static constexpr uint64_t golden_ratio = 0x9e3779b97f4a7c15u;

void rng_jitter_init(rng_jitter_state *state, uint64_t first_counter) {
  *state = (rng_jitter_state){};
  for (size_t i = 0; i < rng_jitter_lanes; i++) {
    state->pool[i] = fnv64_offset ^ ((uint64_t)i * golden_ratio);
  }
  state->pool[0] ^= first_counter;
}

uint8_t rng_jitter_scratch_byte(const rng_jitter_state *state, size_t sample) {
  return (uint8_t)(state->pool[sample & (rng_jitter_lanes - 1)] >>
                   (sample & 7));
}

void rng_jitter_absorb(rng_jitter_state *state, size_t sample, uint64_t delta) {
  if (delta != state->last_delta) {
    state->distinct++;
  }
  state->last_delta = delta;
  /* Fold the low byte of the delta into a rotating lane. */
  const size_t lane = sample & (rng_jitter_lanes - 1);
  state->pool[lane] ^= delta & 0xff;
  state->pool[lane] *= fnv64_prime;
}

bool rng_jitter_healthy(const rng_jitter_state *state) {
  return state->distinct >= rng_jitter_min_distinct;
}

size_t rng_jitter_finish(rng_jitter_state *state, uint8_t *buf, size_t len) {
  size_t out = 0;
  if (rng_jitter_healthy(state) && len > 0) {
    out = len < sizeof(state->pool) ? len : sizeof(state->pool);
    memcpy(buf, state->pool, out);
  }
  memset(state, 0, sizeof(*state));
  return out;
}
