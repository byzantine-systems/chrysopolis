/*
 * Chrysopolis CSPRNG, a client-side HMAC-DRBG (SHA-256, NIST SP 800-90A, via
 * BearSSL) living in beam_server, seeded from an AArch64 jitter source
 * (CNTPCT_EL0) and reseedable through a pluggable entropy-provider interface.
 *
 * The DRBG construction and its conditioning are BearSSL's (a vetted embedded
 * crypto library, MIT-licensed, no OS/malloc dependencies), we deliberately do
 * NOT hand-roll the CSPRNG. What is ours is only the parts that MUST be:
 * reading AArch64 jitter to seed it, the health check / documented fallback,
 * and the per-boot wall-clock offset.
 *
 * This is the portable software floor: it compiles in unconditionally, needs no
 * new PD and no QEMU device, and works on real hardware and any hypervisor
 * (jitter is the always-present source). virtio-rng and a future ARMv8.5
 * RNDR/RNDRRS source are optional *reseed* providers that slot into the same
 * rng_provider_t interface with no structural change.
 *
 * The hot path (rng_fill) always reads the local DRBG, so it never blocks and
 * never runs dry. Providers feed reseed only, and a provider that returns no
 * bytes simply skips a reseed round. beam_server never blocks on another PD to
 * produce randomness (a property worth keeping even now that init() returns to
 * the event loop and blocking waits exist).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * An entropy provider. collect() fills up to len bytes of raw entropy into buf
 * and returns the number of bytes actually produced. A return of 0 means
 * "nothing available this round" and rng_reseed_from() treats it as a no-op
 * (skip the reseed, the DRBG keeps producing from its current key).
 */
typedef struct {
  const char *name;
  size_t (*collect)(uint8_t *buf, size_t len);
} rng_provider_t;

/*
 * Seed the DRBG from the built-in jitter provider (with a documented fallback
 * on health-check failure), srand() the libc PRNG so stray rand() users stop
 * being boot-deterministic, derive the per-boot CLOCK_REALTIME offset, and
 * print one boot line: "RNG|source=<jitter|fallback>|fp=<8-hex>". Call once,
 * after bringup_register_syscalls() (so the timer client config is valid).
 */
void rng_init(void);

/* Fill buf with len cryptographically-strong bytes. Never blocks, never EOFs,
 * never returns short. Safe to call from any syscall shim on this PD. */
void rng_fill(uint8_t *buf, size_t len);

/* Fold a provider's entropy into the DRBG key and rekey. A provider that
 * returns 0 bytes is a no-op. Not used on the "software-only" path, it
 * is the seam the virtio-rng / RNDR reseed providers plug into. */
void rng_reseed_from(const rng_provider_t *provider);

/*
 * Per-boot CLOCK_REALTIME offset in seconds, derived from the DRBG at rng_init.
 * Small and bounded (see rng.c): enough to make erlang:make_ref()/rand differ
 * across boots without making the wall clock randomly wrong by up to a year.
 * bringup.c's clock_gettime shim adds this (over the base epoch below) to
 * CLOCK_REALTIME only. CLOCK_MONOTONIC is untouched. Zero until rng_init runs.
 */
extern uint32_t rng_realtime_offset_sec;

/*
 * 2026-01-01T00:00:00Z. CLOCK_REALTIME = sDDF monotonic time + this base epoch
 * + rng_realtime_offset_sec (bringup.c's clock_gettime shim). Shared here
 * because every consumer of an ABSOLUTE realtime deadline must know the two
 * clocks now differ by ~this much: LionsOS used to alias CLOCK_REALTIME to
 * CLOCK_MONOTONIC, and code that compared a realtime deadline against the
 * monotonic clock (process.c pthread_cond_timedwait, bringup.c
 * clock_nanosleep TIMER_ABSTIME) silently never expired once the epoch went
 * in, which wedged ERTS boot.
 */
#define RNG_REALTIME_BASE_EPOCH 1767225600ull
