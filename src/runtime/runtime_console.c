/*
 * Serial console input for stdin.
 *
 * The libc descriptor table creates stdin with O_RDONLY and no read callback.
 * This unit supplies one that dequeues from the serial RX queue.
 *
 * Reads echo each byte back. QEMU's -serial mon:stdio host terminal runs in
 * raw mode and does not echo, and ERTS runs in dumb-terminal mode because its
 * prim_tty line editor cannot initialise through the ioctl stubs, so without
 * this the user types blind. The echo is cooked: CR or LF prints CRLF, since
 * Enter over serial arrives as '\r', and DEL or BS erases the last glyph.
 */
#include "runtime_console.h"
#include "runtime_config.h"

#include <lions/posix/fd.h>

#include <libmicrokitco.h>
#include <microkit.h>
#include <sddf/serial/queue.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

static void console_echo(char c) {
  switch (c) {
  case '\r':
  case '\n':
    serial_enqueue_batch(&serial_tx_queue_handle, 2, "\r\n");
    break;
  case 0x7f: /* DEL */
  case 0x08: /* BS  */
    serial_enqueue_batch(&serial_tx_queue_handle, 3, "\b \b");
    break;
  default:
    serial_enqueue(&serial_tx_queue_handle, c);
    break;
  }
  microkit_notify(serial_config.tx.id);
}

/* A blocking read (the stdin default) waits on the RX channel while the queue
 * is empty so the RX virtualiser can refill it. A non-blocking read, set via
 * fcntl, returns -EAGAIN instead. A read that already has bytes returns them
 * without waiting for more. */
static ssize_t console_read(void *data, size_t count, int fd) {
  char *buf = data;
  size_t n = 0;

  const fd_entry_t *e = posix_fd_entry(fd);
  const bool nonblock = e != nullptr && (e->flags & O_NONBLOCK);

  while (n < count) {
    char c = 0;
    if (serial_dequeue(&serial_rx_queue_handle, &c) == 0) {
      buf[n++] = c;
      console_echo(c);
    } else {
      if (n > 0) {
        break;
      }
      if (nonblock) {
        return -EAGAIN;
      }
      microkit_cothread_wait_on_channel(serial_config.rx.id);
    }
  }
  return (ssize_t)n;
}

bool runtime_console_readable(void) {
  return serial_queue_length_consumer(&serial_rx_queue_handle) > 0;
}

void runtime_console_attach_stdin(void) {
  fd_entry_t *stdin_entry = posix_fd_entry(STDIN_FILENO);
  if (stdin_entry != nullptr) {
    stdin_entry->read = console_read;
  }
}
