#ifndef CHRYSOPOLIS_RUNTIME_TRACE_H
#define CHRYSOPOLIS_RUNTIME_TRACE_H 1

#include "runtime_trace_ring.h"

#include <stdint.h>

/*
 * Wake-path trace, built in only with `zig build -Dtrace=true`, which defines
 * CHRYSO_TRACE=1. Otherwise every hook is a dead branch the optimizer
 * removes, and no storage or device exists. Hooks only store a record
 * (runtime_trace_ring.h): no printing, locking or allocation, so they are
 * safe inside the syscall emulation.
 *
 * /dev/chrysotrace, O_RDWR, traced builds only: write "1" to arm (clears the
 * ring), "0" to stop; each open reads one snapshot, then EOF. Snapshot:
 * 32-byte header (magic "CHTRACE1", u64 counter frequency, u32 length, u32
 * dropped, u8 state, zero padding) followed by `length` 32-byte records.
 * Short read buffer -EMSGSIZE.
 */

#ifndef CHRYSO_TRACE
#define CHRYSO_TRACE 0
#endif

static constexpr bool runtime_trace_enabled = CHRYSO_TRACE;

/* Store one record if armed. Defined in every build; empty unless traced. */
void runtime_trace_store(runtime_trace_event event, int64_t a, int64_t b);

static inline void runtime_trace(runtime_trace_event event, int64_t a,
                                 int64_t b) {
  if (runtime_trace_enabled) {
    runtime_trace_store(event, a, b);
  }
}

/* True for the trace device's path; always false in untraced builds. */
[[__nodiscard__]] bool runtime_trace_handles_path(const char *path);

/* Open the trace device. -ENOENT in untraced builds, -EACCES unless O_RDWR. */
[[__nodiscard__]] int runtime_trace_open(int flags);

#endif
