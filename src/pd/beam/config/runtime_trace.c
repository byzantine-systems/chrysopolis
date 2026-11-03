/*
 * Trace storage and /dev/chrysotrace (runtime_trace.h). Untraced builds keep
 * only the empty entry points, so the hooks still link and nothing else
 * ships.
 */
#include "runtime_trace.h"

#include <errno.h>

#if CHRYSO_TRACE

#include "runtime_fd.h"

#include <libmicrokitco.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "records are copied out in wire layout");

static constexpr size_t trace_header_size = 32;
/* A full snapshot is exactly 64 KiB, one raw-file read from Erlang. */
static constexpr size_t trace_capacity =
    (65536 - trace_header_size) / sizeof(runtime_trace_record);
static constexpr size_t trace_snapshot_size =
    trace_header_size + trace_capacity * sizeof(runtime_trace_record);

/* Cooperative cothreads on one core: a hook runs to completion. */
static runtime_trace_ring trace_ring;
static runtime_trace_record trace_records[trace_capacity];

static uint64_t read_counter(void) {
  uint64_t v = 0;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
  return v;
}

static uint64_t read_frequency(void) {
  uint64_t v = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}

void runtime_trace_store(runtime_trace_event event, int64_t a, int64_t b) {
  if (trace_ring.state == runtime_trace_off) {
    return;
  }
  const runtime_trace_record record = {
      .ticks = read_counter(),
      .a = a,
      .b = b,
      .event = event,
      .cothread = (int8_t)microkit_cothread_my_handle(),
  };
  runtime_trace_ring_push(&trace_ring, trace_records, trace_capacity, &record);
}

static void put_u64(unsigned char *out, uint64_t v) {
  for (size_t i = 0; i < 8; i++) {
    out[i] = (unsigned char)(v >> (8 * i));
  }
}

static void put_u32(unsigned char *out, uint32_t v) {
  for (size_t i = 0; i < 4; i++) {
    out[i] = (unsigned char)(v >> (8 * i));
  }
}

static ssize_t trace_read(void *data, size_t count, int fd) {
  fd_entry_t *e = posix_fd_entry(fd);
  if (e == nullptr || e->file_ptr < 0) {
    return -EBADF;
  }
  if (e->file_ptr != 0) {
    return 0;
  }
  const size_t records = trace_ring.length * sizeof(runtime_trace_record);
  const size_t total = trace_header_size + records;
  if (count < total) {
    return -EMSGSIZE;
  }
  unsigned char *out = data;
  memset(out, 0, trace_header_size);
  memcpy(out, "CHTRACE1", 8);
  put_u64(out + 8, read_frequency());
  put_u32(out + 16, trace_ring.length);
  put_u32(out + 20, trace_ring.dropped);
  out[24] = trace_ring.state;
  /* Records are stored in wire layout on this little-endian target. */
  if (records != 0) {
    memcpy(out + trace_header_size, trace_records, records);
  }
  e->file_ptr = (off_t)total;
  return (ssize_t)total;
}

static ssize_t trace_write(const void *data, size_t count, int fd) {
  (void)fd;
  if (count == 0) {
    return 0;
  }
  switch (*(const unsigned char *)data) {
  case '1':
    runtime_trace_ring_arm(&trace_ring);
    return (ssize_t)count;
  case '0':
    runtime_trace_ring_disarm(&trace_ring);
    return (ssize_t)count;
  default:
    return -EINVAL;
  }
}

static int trace_fstat(int fd, struct stat *st) {
  (void)fd;
  *st = (struct stat){.st_mode = S_IFCHR | 0600,
                      .st_nlink = 1,
                      .st_size = (off_t)trace_snapshot_size};
  return 0;
}

bool runtime_trace_handles_path(const char *path) {
  return path != nullptr && (strcmp(path, "/dev/chrysotrace") == 0 ||
                             strcmp(path, "dev/chrysotrace") == 0);
}

int runtime_trace_open(int flags) {
  if ((flags & O_ACCMODE) != O_RDWR) {
    return -EACCES;
  }
  return runtime_fd_alloc(trace_read, trace_write, trace_fstat, O_RDWR);
}

#else

void runtime_trace_store(runtime_trace_event event, int64_t a, int64_t b) {
  (void)event;
  (void)a;
  (void)b;
}

bool runtime_trace_handles_path(const char *path) {
  (void)path;
  return false;
}

int runtime_trace_open(int flags) {
  (void)flags;
  return -ENOENT;
}

#endif
