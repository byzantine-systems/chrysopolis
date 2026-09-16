/*
 * Entropy syscalls and virtual device paths.
 *
 * libc_init and libc_init_file claim the getrandom and openat slots before
 * registration runs, and libc_define_syscall asserts a slot is empty, so these
 * handlers are installed with libc_redefine_syscall. That LionsOS libc patch,
 * applied in modules/lionsos.nix, replaces a claimed slot and returns the old
 * handler for chaining. rng.c holds the DRBG.
 *
 * getrandom fills from the DRBG, replacing LionsOS's unseeded rand() loop.
 *
 * openat intercepts /dev/urandom and /dev/random with DRBG-backed descriptors
 * and, in the restart image, the /dev/pd-restart trigger. Every other path
 * chains to the libc filesystem path (file.c sys_openat, then the FAT
 * fs_server). The disk image deliberately omits ::/dev/urandom, so without the
 * intercept those reads would reach fatfs and see EOF.
 */
#include "rng.h"
#include "runtime_fd.h"
#include "runtime_pd_restart.h"
#include "runtime_syscall_handlers.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

static muslcsys_syscall_t previous_openat;

void runtime_sys_openat_chain(muslcsys_syscall_t previous) {
  previous_openat = previous;
}

long runtime_sys_getrandom(va_list ap) {
  void *buf = runtime_sys_arg_pointer(va_arg(ap, long));
  const size_t buflen = runtime_sys_arg_size(va_arg(ap, long));
  /* GRND_NONBLOCK and GRND_RANDOM are moot: the DRBG never blocks and never
   * runs dry. */
  (void)va_arg(ap, long); /* flags */
  if (buf == nullptr) {
    return -EFAULT;
  }
  rng_fill(buf, buflen);
  return (long)buflen;
}

/* Never reports EOF. */
static ssize_t urandom_read(void *data, size_t count, int fd) {
  (void)fd;
  rng_fill(data, count);
  return (ssize_t)count;
}

/* io.c's sys_fstat calls fd_entry->fstat without a null check, so this must be
 * set. A read-only character device like the real /dev/urandom. S_IFSOCK would
 * make the poll handlers treat it as a socket. */
static int urandom_fstat(int fd, struct stat *st) {
  (void)fd;
  *st = (struct stat){};
  st->st_mode = S_IFCHR | 0444;
  st->st_nlink = 1;
  return 0;
}

/* Matches the absolute and the cwd-relative form. cwd is always "/". */
static bool is_rng_device_path(const char *path) {
  return strcmp(path, "/dev/urandom") == 0 ||
         strcmp(path, "/dev/random") == 0 || strcmp(path, "dev/urandom") == 0 ||
         strcmp(path, "dev/random") == 0;
}

long runtime_sys_openat(va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  (void)va_arg(ap, long); /* dirfd */
  const char *path = runtime_sys_arg_pointer(va_arg(ap, long));

  if (path != nullptr && is_rng_device_path(path)) {
    va_end(copy);
    return runtime_fd_alloc(urandom_read, nullptr, urandom_fstat, O_RDONLY);
  }

  /* In a production image this is false for every path, which then falls
   * through to the filesystem and reports ENOENT. */
  if (path != nullptr && runtime_pd_restart_handles_path(path)) {
    va_end(copy);
    return runtime_pd_restart_open();
  }

  const long ret = previous_openat != nullptr ? previous_openat(copy) : -ENOSYS;
  va_end(copy);
  return ret;
}
