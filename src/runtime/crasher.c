/*
 * crasher.c - test-only fault-injection PD for the Root fault handler.
 *
 * A deliberately faulting child of the Root PD, used only by the restart-smoke
 * integration test. It is NOT staged into the production images (default /
 * test-image); only the dedicated restart image references it (built with
 * -Dwith-crasher and the --with-crasher SDF, see modules/images.nix).
 *
 * On every init() it faults (writes through a NULL pointer), so Root's fault()
 * handler catches it, restarts it up to the restart budget, then gives up and
 * stops it. It prints an init counter kept in .bss BEFORE faulting, which:
 *   - proves the PD actually re-executes from a clean entry after each restart
 *     (the counter climbs: n=1, 2, 3, ...), and
 *   - demonstrates that .bss is NOT re-zeroed on a warm restart (the Microkit
 *     loader zeroes it once, at boot; microkit_pd_restart only rewrites PC), so
 *     the counter persists across restarts rather than resetting to 0. That
 *     persistence is exactly why real drivers must re-initialise idempotently.
 *
 * The whole system must survive this (Root absorbs the faults, boot continues
 * to the Eshell), which is the point the test asserts.
 */
#include <microkit.h>

/* Persists across a warm restart (see file header). */
static unsigned int init_count;

static void put_dec(unsigned int v) {
  char buf[11];
  unsigned int i = sizeof(buf);
  buf[--i] = '\0';
  do {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  microkit_dbg_puts(&buf[i]);
}

void init(void) {
  init_count++;
  microkit_dbg_puts("CRASHER|init|n=");
  put_dec(init_count);
  microkit_dbg_puts("\n");

  /* Fault so Root's fault() handler runs. */
  volatile int *p = 0;
  *p = 0;
}

void notified(microkit_channel ch) { (void)ch; }
