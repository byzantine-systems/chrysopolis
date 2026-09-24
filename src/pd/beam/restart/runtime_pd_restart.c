/*
 * Test-only driver-restart trigger: /dev/pd-restart.
 *
 * Writing a driver class name to this virtual device asks Root to restart that
 * healthy driver. Prefixing the class with "fault:" asks Root to resume the
 * real driver at address 0 instead, which produces a genuine seL4 instruction
 * fault that Root then handles through its ordinary fault path.
 *
 * Every beam_server contains this code, since production images reuse the same
 * ELF and only the SDF differs. Data gates it: the channel ids live in the
 * .pd_restart_config section below, and modules/images.nix patches real ids
 * in only for the restart image. Production images keep the
 * PD_RESTART_CH_NONE initialiser, so the path does not exist there and this
 * code never asks Microkit to notify a channel the SDF did not wire. Microkit
 * 2.3 rejects such a notification after printing a diagnostic in debug builds,
 * but it is a silent no-op in release builds, which would falsely report a
 * successful restart request to the writer.
 */
#include "runtime_pd_restart.h"
#include "runtime_config.h"
#include "runtime_fd.h"
#include "runtime_pd_restart_parse.h"

#include <microkit.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* One row per operation (healthy restart, then fault injection) and one column
 * per restartable driver class, in the order tools/sdf/system.zig wires the
 * beam_server to Root test channels. The object has its own section and is
 * volatile and used, so the compiler cannot fold the initialiser away and
 * objcopy can overwrite it. Root's .restart_config and sDDF's per-PD config
 * blobs use the same mechanism. */
__attribute__((__section__(PD_RESTART_CONFIG_SECTION), used)) volatile uint8_t
    pd_restart_channels[PD_RESTART_MODE_COUNT][PD_RESTART_CLASS_COUNT] = {
        {
            PD_RESTART_CH_NONE, /* healthy serial */
            PD_RESTART_CH_NONE, /* healthy timer */
            PD_RESTART_CH_NONE, /* healthy blk */
            PD_RESTART_CH_NONE, /* healthy eth */
        },
        {
            PD_RESTART_CH_NONE, /* fault serial */
            PD_RESTART_CH_NONE, /* fault timer */
            PD_RESTART_CH_NONE, /* fault blk */
            PD_RESTART_CH_NONE, /* fault eth */
        },
};

/* Class names accepted as the write payload, indexed like the columns of
 * pd_restart_channels. */
static const char *const pd_restart_names[PD_RESTART_CLASS_COUNT] = {
    "serial", "timer", "blk", "eth"};

static_assert(PD_RESTART_MODE_HEALTHY == runtime_pd_restart_mode_healthy);
static_assert(PD_RESTART_MODE_FAULT == runtime_pd_restart_mode_fault);

/* The payload is a class name for a healthy restart or "fault:<class>" for a
 * genuine driver fault. Trailing whitespace is ignored so file:write_file/2
 * and an echo-style write with a newline both match. */
static ssize_t pd_restart_write(const void *data, size_t count, int fd) {
  (void)fd;
  size_t mode = 0;
  size_t class_index = 0;
  if (runtime_pd_restart_parse(data, count, pd_restart_names,
                               PD_RESTART_CLASS_COUNT, &mode,
                               &class_index) != runtime_pd_restart_parse_ok) {
    /* An unknown class fails the write, so a typo in a test surfaces as an
     * error at the write. */
    return -EINVAL;
  }

  /* The class needs a channel in THIS image. A missing one means the SDF was
   * generated without --with-restart-debug for this class. */
  const uint8_t ch = pd_restart_channels[mode][class_index];
  if (ch == PD_RESTART_CH_NONE) {
    return -ENODEV;
  }
  /* Root owns the restart policy and the child TCB caps. The full original
   * count is reported: the trimmed bytes were consumed, and a short write
   * would make a caller such as file:write/2 retry with the remainder and
   * trigger a second restart. */
  printf("PD_RESTART|request|class=%s|mode=%s|ch=%u\n",
         pd_restart_names[class_index],
         mode == PD_RESTART_MODE_FAULT ? "fault" : "restart", (unsigned)ch);
  microkit_notify(ch);
  return (ssize_t)count;
}

/* io.c's sys_fstat calls fd_entry->fstat without a null check, so every
 * descriptor that can reach it needs one. A write-only character device.
 * S_IFSOCK would make the poll handlers treat it as a socket. */
static int pd_restart_fstat(int fd, struct stat *st) {
  (void)fd;
  *st = (struct stat){};
  st->st_mode = S_IFCHR | 0222;
  st->st_nlink = 1;
  return 0;
}

/* Matches the absolute and the cwd-relative form. cwd is always "/". */
static bool is_pd_restart_path(const char *path) {
  return strcmp(path, "/dev/pd-restart") == 0 ||
         strcmp(path, "dev/pd-restart") == 0;
}

bool runtime_pd_restart_handles_path(const char *path) {
  return is_pd_restart_path(path) &&
         runtime_pd_restart_any_channel(
             PD_RESTART_MODE_COUNT, PD_RESTART_CLASS_COUNT, pd_restart_channels,
             PD_RESTART_CH_NONE);
}

int runtime_pd_restart_open(void) {
  return runtime_fd_alloc(nullptr, pd_restart_write, pd_restart_fstat,
                          O_WRONLY);
}
