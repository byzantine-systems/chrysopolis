/* rng_select.h: seed source choice, reseed gating, realtime offset and the
 * jitter health rule. Every sample here is synthetic, and no check prints
 * pool contents. */
#include "check.h"

#include "rng_select.h"

#include <stdint.h>
#include <string.h>

enum : size_t { samples = 1024, pool_bytes = rng_jitter_lanes * 8 };

static void test_seed_selection(void) {
  CHECK(rng_select_seed(0) == rng_seed_source_fallback);
  CHECK(rng_select_seed(rng_seed_min_bytes - 1) == rng_seed_source_fallback);
  CHECK(rng_select_seed(rng_seed_min_bytes) == rng_seed_source_jitter);
  CHECK(rng_select_seed(SIZE_MAX) == rng_seed_source_jitter);
  CHECK(strcmp(rng_seed_source_name(rng_seed_source_jitter), "jitter") == 0);
  CHECK(strcmp(rng_seed_source_name(rng_seed_source_fallback), "fallback") ==
        0);
}

static size_t empty_collect(uint8_t *buf, size_t len) {
  (void)buf;
  (void)len;
  return 0;
}

static void test_reseed_gating(void) {
  const rng_provider_t provider = {.name = "test", .collect = empty_collect};
  const rng_provider_t no_collect = {.name = "test"};
  CHECK(rng_reseed_applies(true, &provider));
  CHECK(!rng_reseed_applies(false, &provider));
  CHECK(!rng_reseed_applies(true, nullptr));
  CHECK(!rng_reseed_applies(true, &no_collect));
}

static void test_realtime_offset(void) {
  CHECK(rng_realtime_offset(0) == 0);
  CHECK(rng_realtime_offset(899) == 899);
  CHECK(rng_realtime_offset(900) == 0);
  CHECK(rng_realtime_offset(UINT32_MAX) == UINT32_MAX % 900);
  for (uint32_t raw = UINT32_MAX - 2000; raw != 0; raw++) {
    if (rng_realtime_offset(raw) >= rng_realtime_offset_modulo) {
      CHECK(false);
    }
  }
}

/* Run a full collection over a delta sequence and finish into buf. */
static size_t collect(uint64_t (*delta_at)(size_t), uint8_t *buf, size_t len,
                      rng_jitter_state *state) {
  rng_jitter_init(state, 12345);
  for (size_t i = 0; i < samples; i++) {
    (void)rng_jitter_scratch_byte(state, i);
    rng_jitter_absorb(state, i, delta_at(i));
  }
  return rng_jitter_finish(state, buf, len);
}

static uint64_t lockstep_delta(size_t i) {
  (void)i;
  return 1000;
}

static uint64_t varying_delta(size_t i) { return 1000 + (i * 7919) % 13; }

static bool state_is_wiped(const rng_jitter_state *state) {
  const rng_jitter_state zero = {};
  return memcmp(state, &zero, sizeof(zero)) == 0;
}

/* A lockstep counter fails the health check: nothing is emitted, the output
 * buffer is untouched, and the pool is still wiped. */
static void test_lockstep_counter_fails(void) {
  uint8_t buf[pool_bytes] = {};
  memset(buf, 0xee, sizeof(buf));
  rng_jitter_state state = {};
  CHECK_EQ_U64(collect(lockstep_delta, buf, sizeof(buf), &state), 0);
  CHECK(state_is_wiped(&state));
  for (size_t i = 0; i < sizeof(buf); i++) {
    CHECK(buf[i] == 0xee);
  }
}

/* The first delta differs from the initial zero, so a constant sequence
 * scores exactly one distinct delta. */
static void test_distinct_count_boundary(void) {
  rng_jitter_state state = {};
  rng_jitter_init(&state, 0);
  for (size_t i = 0; i < samples; i++) {
    rng_jitter_absorb(&state, i, 5);
  }
  CHECK(state.distinct == 1);
  CHECK(!rng_jitter_healthy(&state));

  /* Alternating deltas: every sample differs from its predecessor. Stop one
   * short of the floor, then reach it. */
  rng_jitter_init(&state, 0);
  for (size_t i = 0; i < rng_jitter_min_distinct - 1; i++) {
    rng_jitter_absorb(&state, i, i % 2 + 1);
  }
  CHECK(!rng_jitter_healthy(&state));
  rng_jitter_absorb(&state, rng_jitter_min_distinct - 1,
                    (rng_jitter_min_distinct - 1) % 2 + 1);
  CHECK(rng_jitter_healthy(&state));
}

static void test_healthy_emit_lengths(void) {
  rng_jitter_state state = {};
  uint8_t big[pool_bytes + 16] = {};
  memset(big, 0xee, sizeof(big));
  CHECK_EQ_U64(collect(varying_delta, big, sizeof(big), &state), pool_bytes);
  CHECK(state_is_wiped(&state));
  /* Bytes past the pool size are never written. */
  for (size_t i = pool_bytes; i < sizeof(big); i++) {
    CHECK(big[i] == 0xee);
  }

  uint8_t small[16] = {};
  CHECK_EQ_U64(collect(varying_delta, small, sizeof(small), &state), 16);
  /* A shorter request is a prefix of the same pool. */
  CHECK(memcmp(small, big, sizeof(small)) == 0);

  CHECK_EQ_U64(collect(varying_delta, nullptr, 0, &state), 0);
  CHECK(state_is_wiped(&state));
}

/* Identical samples fold identically; a single changed delta changes the
 * pool. */
static void test_fold_is_sample_sensitive(void) {
  rng_jitter_state state = {};
  uint8_t first[pool_bytes] = {};
  uint8_t second[pool_bytes] = {};
  CHECK_EQ_U64(collect(varying_delta, first, sizeof(first), &state),
               pool_bytes);
  CHECK_EQ_U64(collect(varying_delta, second, sizeof(second), &state),
               pool_bytes);
  CHECK(memcmp(first, second, sizeof(first)) == 0);

  rng_jitter_init(&state, 12345);
  for (size_t i = 0; i < samples; i++) {
    rng_jitter_absorb(&state, i, varying_delta(i) + (i == 500 ? 1 : 0));
  }
  CHECK_EQ_U64(rng_jitter_finish(&state, second, sizeof(second)), pool_bytes);
  CHECK(memcmp(first, second, sizeof(first)) != 0);
}

static void test_scratch_byte(void) {
  rng_jitter_state state = {};
  rng_jitter_init(&state, 0);
  state.pool[0] = 0x0123456789abcdefu;
  state.pool[3] = 0xff00u;
  CHECK(rng_jitter_scratch_byte(&state, 0) == 0xef);
  CHECK(rng_jitter_scratch_byte(&state, 8) == 0xef);
  /* Sample 3 reads lane 3 shifted right by 3. */
  CHECK(rng_jitter_scratch_byte(&state, 3) == (uint8_t)(0xff00u >> 3));
}

int main(void) {
  test_seed_selection();
  test_reseed_gating();
  test_realtime_offset();
  test_lockstep_counter_fails();
  test_distinct_count_boundary();
  test_healthy_emit_lengths();
  test_fold_is_sample_sensitive();
  test_scratch_byte();
  return check_finish("rng_select");
}
