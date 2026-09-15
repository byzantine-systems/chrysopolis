#ifndef CHRYSOPOLIS_RUNTIME_CONSOLE_H
#define CHRYSOPOLIS_RUNTIME_CONSOLE_H 1

#include <stdbool.h>

/*
 * True when the serial RX queue holds at least one unread byte, i.e. stdin
 * would not block. The serial queues must already be initialised. This only
 * inspects the queue and never consumes input.
 */
[[__nodiscard__]] bool runtime_console_readable(void);

/*
 * Give the libc stdin descriptor its missing read callback, which dequeues
 * from the serial RX queue and echoes each byte. Call after libc_init() has
 * set up the descriptor table. A missing stdin entry is left untouched.
 */
void runtime_console_attach_stdin(void);

#endif
