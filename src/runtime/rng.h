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
#ifndef CHRYSOPOLIS_RUNTIME_RNG_H
#define CHRYSOPOLIS_RUNTIME_RNG_H 1

#include <stddef.h>
#include <stdint.h>

/*
 * An entropy provider borrowed by rng_reseed_from() for one synchronous call.
 * name may be NULL and is not secret. collect must be non-NULL, fills at most
 * len bytes of raw entropy into non-NULL buf, and returns a value in [0, len].
 * A return of 0 means "nothing available this round" and is a no-op.
 * Providers must not log raw entropy or retain buf.
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
 * after runtime_syscalls_register() and before any generation or reseed call.
 * The fingerprint is derived output; seeds, raw entropy, DRBG state and random
 * output must never be logged.
 */
void rng_init(void);

/* Fill buf with len cryptographically-strong bytes. buf must be non-NULL when
 * len is non-zero and rng_init() must already have completed. Never blocks,
 * never EOFs, never returns short, and does not retain buf. */
void rng_fill(uint8_t *buf, size_t len);

/* Fold a borrowed provider's entropy into the DRBG key and rekey. NULL, a NULL
 * collect callback, a call before rng_init(), or a provider returning 0 bytes
 * is a no-op. The call is synchronous and neither retains the provider nor
 * blocks beyond the provider's collect callback. */
void rng_reseed_from(const rng_provider_t *provider);

/*
 * Per-boot CLOCK_REALTIME offset in seconds, derived from the DRBG at rng_init.
 * Small and bounded (see rng.c): enough to make erlang:make_ref()/rand differ
 * across boots without making the wall clock randomly wrong by up to a year.
 * runtime_sys_clock_gettime adds this (over the base epoch below) to
 * CLOCK_REALTIME only. CLOCK_MONOTONIC is untouched. Zero until rng_init runs.
 */
extern uint32_t rng_realtime_offset_sec;

/*
 * 2026-01-01T00:00:00Z. CLOCK_REALTIME = sDDF monotonic time + this base epoch
 * + rng_realtime_offset_sec (runtime_sys_clock_gettime). Shared here
 * because every consumer of an ABSOLUTE realtime deadline must know the two
 * clocks now differ by ~this much: LionsOS used to alias CLOCK_REALTIME to
 * CLOCK_MONOTONIC, and code that compared a realtime deadline against the
 * monotonic clock (process.c pthread_cond_timedwait, runtime_sys_time.c
 * clock_nanosleep TIMER_ABSTIME) silently never expired once the epoch went
 * in, which wedged ERTS boot.
 */
#define RNG_REALTIME_BASE_EPOCH 1767225600ull

#endif
