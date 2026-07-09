/*
 * Seeding + reseed glue around BearSSL's HMAC-DRBG (SHA-256). See rng.h for the
 * design rationale (portable software floor, pluggable reseed, no hand-rolled
 * crypto). The DRBG itself, instantiate, generate, reseed, and all the
 * conditioning, is BearSSL's br_hmac_drbg (NIST SP 800-90A). This file only
 * provides the entropy BearSSL cannot: AArch64 timing jitter, the health check,
 * and a documented fallback.
 *
 * Entropy quality note (documented per the acceptance criteria):
 *   - The seed comes from CNTPCT_EL0 timing jitter across a memory-perturbing
 *     loop. On real silicon and on any hypervisor that does NOT force
 *     deterministic time (QEMU without -icount, which is our config) the guest
 *     counter tracks host wall-clock time, so the deltas genuinely jitter with
 *     host scheduling and cache state. HMAC-DRBG conditions the raw samples
 *     (they are fed straight to br_hmac_drbg_init, which HMACs them), so no
 *     hand-rolled whitening is needed.
 *   - Under QEMU TCG with -icount, or on a board whose counter is lockstep, the
 *     jitter health check (distinct-delta count) fails and we fall back to a
 *     documented mix (build-time constant XOR initial counter XOR sDDF timer),
 *     marking the source "fallback" in the boot print. The fallback still
 *     varies per boot under normal QEMU because the counter/timer advance.
 *   - crypto:strong_rand_bytes is NOT backed by this: the crypto NIF is
 *     unlinked (micro-openssl.a = md5 only). That is a separate future issue
 *     (link libcrypto + ship the crypto app).
 */
#include "rng.h"

#include <bearssl.h>

#include <sddf/timer/client.h>
#include <sddf/timer/config.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Timer client config (driver_id channel), defined in main.c. Used only for
 * the documented fallback seed and to perturb the jitter loop. */
extern timer_client_config_t timer_config;

uint32_t rng_realtime_offset_sec = 0;

/* The one DRBG instance for this PD. br_hmac_drbg is a plain struct (no heap),
 * safe as a file-static. */
static br_hmac_drbg_context g_drbg;
static int g_drbg_ready;

void rng_fill(uint8_t *buf, size_t len) {
  br_hmac_drbg_generate(&g_drbg, buf, len);
}

void rng_reseed_from(const rng_provider_t *provider) {
  if (!g_drbg_ready || provider == NULL || provider->collect == NULL) {
    return;
  }
  uint8_t raw[64];
  size_t n = provider->collect(raw, sizeof(raw));
  if (n == 0) {
    return; /* empty provider: skip this reseed round */
  }
  /* br_hmac_drbg_update ADDS entropy (it never replaces the pool), so folding
   * in even low-quality bytes can only help, exactly the reseed semantics we
   * want for the optional virtio-rng / RNDR providers. */
  br_hmac_drbg_update(&g_drbg, raw, n);
  memset(raw, 0, sizeof(raw));
}

/* ---- Built-in jitter entropy provider (CNTPCT_EL0) ---- */

static inline uint64_t read_cntpct(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;
}

/* FNV-1a mixing constants (64-bit): a cheap, dependency-free accumulator that
 * folds the raw timing samples into the seed pool. HMAC-DRBG does the real
 * conditioning; this just packs the samples. */
#define FNV64_OFFSET 0xcbf29ce484222325ull
#define FNV64_PRIME 0x100000001b3ull

/* Volatile scratch the timing loop churns so successive CNTPCT reads bracket a
 * genuine, cache/scheduler-perturbed store rather than being trivially fused.
 */
static volatile uint8_t jitter_scratch[64];

#define JITTER_SAMPLES 1024
/* Health floor: at least this many of the JITTER_SAMPLES deltas must differ
 * from the previous one. A lockstep counter (e.g. -icount) yields near-constant
 * deltas and trips this, sending rng_init to the documented fallback. */
#define JITTER_MIN_DISTINCT 64

/* Seed pool: 8 lanes = 64 bytes, enough for a generous HMAC-DRBG seed. */
#define JITTER_POOL_LANES 8

/*
 * Collect ~1024 CNTPCT_EL0 deltas, fold them into a 64-byte pool, and (on
 * passing the distinct-delta health check) emit up to len raw pool bytes.
 * Returns 0 on health-check failure so rng_init falls back to the documented
 * mix. No whitening here, HMAC-DRBG conditions whatever we hand it.
 */
static size_t jitter_collect(uint8_t *buf, size_t len) {
  uint64_t pool[JITTER_POOL_LANES];
  for (int i = 0; i < JITTER_POOL_LANES; i++) {
    pool[i] = FNV64_OFFSET ^ ((uint64_t)i * 0x9e3779b97f4a7c15ull);
  }
  pool[0] ^= read_cntpct();

  uint64_t prev = read_cntpct();
  uint64_t last_delta = 0;
  unsigned distinct = 0;

  for (int i = 0; i < JITTER_SAMPLES; i++) {
    jitter_scratch[i & 63] ^=
        (uint8_t)(pool[i & (JITTER_POOL_LANES - 1)] >> (i & 7));
    __asm__ volatile("" ::: "memory");
    uint64_t now = read_cntpct();
    uint64_t delta = now - prev;
    prev = now;
    if (delta != last_delta) {
      distinct++;
    }
    last_delta = delta;
    /* FNV-1a fold of the low byte of the delta into a rotating lane. */
    int lane = i & (JITTER_POOL_LANES - 1);
    pool[lane] ^= (delta & 0xff);
    pool[lane] *= FNV64_PRIME;
  }

  if (distinct < JITTER_MIN_DISTINCT) {
    return 0; /* counter too regular: let rng_init use the fallback */
  }

  size_t out = len < sizeof(pool) ? len : sizeof(pool);
  memcpy(buf, pool, out);
  memset(pool, 0, sizeof(pool));
  return out;
}

/* A fixed compile-time constant folded into the fallback seed. It is NOT the
 * source of per-boot variance (the counter/timer below are), it only ensures
 * the fallback never degenerates to an all-zero seed. Reproducible-build rules
 * (rightly) forbid __DATE__/__TIME__, so this is a plain literal. */
#define RNG_FALLBACK_CONSTANT 0x9e3779b97f4a7c15ull

/*
 * Documented fallback when jitter fails its health check: a build-time constant
 * XOR the initial CNTPCT reading XOR the sDDF monotonic timer. Under normal
 * QEMU the counter/timer still advance per boot, so the seed varies; under
 * strict determinism it is at least defined rather than a fixed pattern.
 * Returns 32 bytes.
 */
static size_t fallback_seed(uint8_t out[32]) {
  uint64_t build = RNG_FALLBACK_CONSTANT;
  uint64_t ctr = read_cntpct();
  uint64_t t = sddf_timer_time_now(timer_config.driver_id);

  uint64_t mix[4];
  mix[0] = build ^ ctr;
  mix[1] = build ^ t;
  mix[2] = ctr ^ (t << 1) ^ (t >> 1);
  mix[3] = (ctr * FNV64_PRIME) ^ t ^ build;
  memcpy(out, mix, sizeof(mix));
  memset(mix, 0, sizeof(mix));
  return 32;
}

static const rng_provider_t jitter_provider = {
    .name = "jitter",
    .collect = jitter_collect,
};

/* Bounded per-boot CLOCK_REALTIME offset: 0..(RNG_OFFSET_MODULO-1) seconds.
 * 15 minutes is plenty to make time-seeded refs/rand differ across boots while
 * keeping the wall clock within minutes of the base epoch (not randomly wrong
 * by up to a year, which a full sub-year offset would be). */
#define RNG_OFFSET_MODULO 900u

void rng_init(void) {
  uint8_t seed[64];
  const char *source;
  size_t seed_len;

  seed_len = jitter_provider.collect(seed, sizeof(seed));
  if (seed_len >= 32) {
    source = jitter_provider.name;
  } else {
    seed_len = fallback_seed(seed);
    source = "fallback";
  }

  br_hmac_drbg_init(&g_drbg, &br_sha256_vtable, seed, seed_len);
  g_drbg_ready = 1;
  memset(seed, 0, sizeof(seed));

  /* Seed the libc PRNG so any stray rand() user is no longer boot-deterministic
   * (LionsOS sys_getrandom's old rand() loop, and lwIP's LWIP_RAND). */
  uint32_t srand_seed;
  rng_fill((uint8_t *)&srand_seed, sizeof(srand_seed));
  srand(srand_seed);

  /* Per-boot wall-clock offset (see rng.h / bringup.c clock shim). */
  uint32_t off;
  rng_fill((uint8_t *)&off, sizeof(off));
  rng_realtime_offset_sec = off % RNG_OFFSET_MODULO;

  /* One functional boot line the rng-smoke test keys off. The fingerprint is
   * fresh DRBG output, so it differs across boots exactly when the seed does.
   */
  uint8_t fp[4];
  rng_fill(fp, sizeof(fp));
  printf("RNG|source=%s|fp=%02x%02x%02x%02x\n", source, fp[0], fp[1], fp[2],
         fp[3]);
}
