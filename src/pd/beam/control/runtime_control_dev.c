/*
 * BEAM-facing Root control devices. Both carry whole generated ABI records
 * (version chryso_abi_version); Erlang never sees a shared-page pointer.
 *
 * /dev/chrysostat, O_RDONLY: one validated root_status snapshot per open,
 * then EOF; reopen to read again. No PPC and no notification wait.
 *   short buffer -EMSGSIZE, torn after retries -EAGAIN, invalid copy -EIO,
 *   page unmapped -ENODEV.
 *
 * /dev/chrysoctl, O_RDWR, one opener: write one CtlCommand, read one
 * CtlReply. The write is one synchronous PPC, which blocks the whole
 * beam_server TCB until Root replies, so callers must pace commands. Root's
 * typed outcomes travel in the reply; errnos are local:
 *   wrong size -EMSGSIZE, checker reject -EBADMSG, already open or reply
 *   pending -EBUSY, read with nothing pending -ENODATA, no valid Root frame
 *   -EIO, PPC id undeclared in this image -ENODEV.
 * close drops a pending reply, so its outcome is unknown until a fresh
 * status read. dup3 is refused so one command never has two owners. Owner
 * state is .bss, which a BEAM restart zeroes.
 */
#include "runtime_control_dev.h"
#include "root_status_reader.h"
#include "runtime_config.h"
#include "runtime_ctl_device.h"
#include "runtime_fd.h"

#include <chrysopolis/system_abi.h>

#include <microkit.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

/* Copy attempts before a status read reports -EAGAIN. */
static constexpr unsigned int stat_attempts = 3;

/* No callback here yields, so cothreads never interleave on these. */
static runtime_ctl_owner ctl_owner;
/* Two buffers: the reader wants aligned pages and the caller's has no
 * alignment guarantee. */
static chryso_root_status_page stat_scratch;
static chryso_root_status_page stat_copy;

static_assert(chryso_control_pp_channel_beam <= MICROKIT_MAX_CHANNEL_ID);
static_assert(chryso_control_status_beam_vaddr %
                  alignof(chryso_root_status_page) ==
              0);

static int ctl_errno(runtime_ctl_status status) {
  switch (status) {
  case runtime_ctl_ok:
    return 0;
  case runtime_ctl_busy:
    return -EBUSY;
  case runtime_ctl_size:
    return -EMSGSIZE;
  case runtime_ctl_malformed:
    return -EBADMSG;
  case runtime_ctl_no_reply:
    return -ENODATA;
  case runtime_ctl_not_owner:
    return -EBADF;
  case runtime_ctl_unavailable:
  case runtime_ctl_out_of_order:
    return -EIO;
  }
  return -EIO;
}

static ssize_t stat_read(void *data, size_t count, int fd) {
  fd_entry_t *e = posix_fd_entry(fd);
  if (e == nullptr || e->file_ptr < 0) {
    return -EBADF;
  }
  switch (runtime_stat_read_decide(count, (uint64_t)e->file_ptr,
                                   sizeof(stat_copy))) {
  case runtime_stat_eof:
    return 0;
  case runtime_stat_short:
    return -EMSGSIZE;
  case runtime_stat_copy:
    break;
  }
  if (root_status_view != chryso_control_status_beam_vaddr) {
    return -ENODEV;
  }
  /* The address is the mapped page, aligned per the static_assert above. */
  switch (root_status_read((const chryso_root_status_page *)root_status_view,
                           &stat_scratch, &stat_copy, sizeof(stat_copy),
                           stat_attempts)) {
  case root_status_read_ok:
    break;
  case root_status_read_torn:
    return -EAGAIN;
  case root_status_read_invalid:
  case root_status_read_size:
    return -EIO;
  }
  memcpy(data, &stat_copy, sizeof(stat_copy));
  e->file_ptr = (off_t)sizeof(stat_copy);
  return (ssize_t)sizeof(stat_copy);
}

/* Not a socket, so the poll handlers treat it as an ordinary descriptor. */
static int stat_fstat(int fd, struct stat *st) {
  (void)fd;
  *st = (struct stat){.st_mode = S_IFCHR | 0444, .st_nlink = 1};
  return 0;
}

static ssize_t ctl_write(const void *data, size_t count, int fd) {
  runtime_ctl_call call = {};
  const runtime_ctl_status admitted =
      runtime_ctl_write(&ctl_owner, fd, data, count, &call);
  if (admitted != runtime_ctl_ok) {
    return ctl_errno(admitted);
  }
  for (size_t i = 0; i < runtime_ctl_words; i++) {
    microkit_mr_set((uint8_t)i, call.args[i]);
  }
  const microkit_msginfo info =
      microkit_ppcall(chryso_control_pp_channel_beam,
                      microkit_msginfo_new(call.opcode, runtime_ctl_words));
  const uint64_t words = microkit_msginfo_get_count(info);
  /* Read MRs only from a full frame, so a short one exposes nothing stale. */
  uint64_t values[runtime_ctl_words] = {};
  if (words == runtime_ctl_words) {
    for (size_t i = 0; i < runtime_ctl_words; i++) {
      values[i] = microkit_mr_get((uint8_t)i);
    }
  }
  const runtime_ctl_status replied = runtime_ctl_reply(
      &ctl_owner, fd, microkit_msginfo_get_label(info), words, values);
  return replied == runtime_ctl_ok ? (ssize_t)count : ctl_errno(replied);
}

static ssize_t ctl_read(void *data, size_t count, int fd) {
  const runtime_ctl_status status =
      runtime_ctl_read(&ctl_owner, fd, data, count);
  return status == runtime_ctl_ok ? (ssize_t)count : ctl_errno(status);
}

static int ctl_fstat(int fd, struct stat *st) {
  (void)fd;
  *st = (struct stat){.st_mode = S_IFCHR | 0600, .st_nlink = 1};
  return 0;
}

/* The descriptor is released even when the owner check fails, so a stale
 * entry can never leak a slot. */
static int ctl_close(int fd) {
  (void)runtime_ctl_close(&ctl_owner, fd);
  return runtime_fd_close(fd);
}

static int ctl_dup3(int oldfd, int newfd) {
  (void)oldfd;
  (void)runtime_fd_close(newfd);
  return -EBUSY;
}

/* Absolute and cwd-relative forms; cwd is always "/". */
static bool is_stat_path(const char *path) {
  return strcmp(path, "/dev/chrysostat") == 0 ||
         strcmp(path, "dev/chrysostat") == 0;
}

static bool is_ctl_path(const char *path) {
  return strcmp(path, "/dev/chrysoctl") == 0 ||
         strcmp(path, "dev/chrysoctl") == 0;
}

bool runtime_control_dev_handles_path(const char *path) {
  return is_stat_path(path) || is_ctl_path(path);
}

static int stat_open(int flags) {
  if ((flags & O_ACCMODE) != O_RDONLY) {
    return -EACCES;
  }
  if (root_status_view != chryso_control_status_beam_vaddr) {
    return -ENODEV;
  }
  return runtime_fd_alloc(stat_read, nullptr, stat_fstat, O_RDONLY);
}

static int ctl_open(int flags) {
  if ((flags & O_ACCMODE) != O_RDWR) {
    return -EACCES;
  }
  /* microkit_ppcall on an undeclared id is a silent no-op in release
   * builds; refuse here instead. */
  if ((microkit_pps & (UINT64_C(1) << chryso_control_pp_channel_beam)) == 0) {
    return -ENODEV;
  }
  const int fd = runtime_fd_alloc_ops(
      &(const runtime_fd_ops){
          .read = ctl_read,
          .write = ctl_write,
          .fstat = ctl_fstat,
          .close = ctl_close,
          .dup3 = ctl_dup3,
      },
      O_RDWR);
  if (fd < 0) {
    return fd;
  }
  const runtime_ctl_status opened = runtime_ctl_open(&ctl_owner, fd);
  if (opened != runtime_ctl_ok) {
    (void)runtime_fd_close(fd);
    return ctl_errno(opened);
  }
  return fd;
}

int runtime_control_dev_open(const char *path, int flags) {
  if (is_stat_path(path)) {
    return stat_open(flags);
  }
  if (is_ctl_path(path)) {
    return ctl_open(flags);
  }
  return -ENOENT;
}
