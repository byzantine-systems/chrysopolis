#ifndef CHRYSOPOLIS_HOST_CHECK_H
#define CHRYSOPOLIS_HOST_CHECK_H 1

/*
 * Minimal assertion support for the host suites. A failed check prints its
 * location and keeps going, so one run reports every broken case.
 * check_finish() turns the tally into the process exit status.
 *
 * Suites must never pass secret material (entropy, seeds, DRBG state) to
 * these helpers: a failure prints the compared values.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static size_t check_total;
static size_t check_failed;

static inline void check_record(bool ok, const char *expr, const char *file,
                                int line) {
  check_total++;
  if (!ok) {
    check_failed++;
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }
}

static inline void check_record_eq_u64(uint64_t got, uint64_t want,
                                       const char *expr, const char *file,
                                       int line) {
  check_total++;
  if (got != want) {
    check_failed++;
    fprintf(stderr,
            "%s:%d: CHECK_EQ_U64 failed: %s\n  got:  %" PRIu64
            "\n  want: %" PRIu64 "\n",
            file, line, expr, got, want);
  }
}

/* Function-like macros only to capture the expression text and location. */
#define CHECK(expr) check_record((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ_U64(got, want)                                                \
  check_record_eq_u64((got), (want), #got " == " #want, __FILE__, __LINE__)

/* Print the tally and return the exit status for main. */
[[nodiscard]] static inline int check_finish(const char *suite) {
  printf("%s: %zu checks, %zu failed\n", suite, check_total, check_failed);
  return check_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
