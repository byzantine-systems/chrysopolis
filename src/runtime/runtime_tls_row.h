#ifndef CHRYSOPOLIS_RUNTIME_TLS_ROW_H
#define CHRYSOPOLIS_RUNTIME_TLS_ROW_H 1

#include <stdbool.h>
#include <stddef.h>

/*
 * POSIX thread-specific data teardown for one thread's value row. Pure: the
 * key table and value rows are caller storage.
 */

typedef struct {
  void (*destructor)(void *);
  bool used;
} runtime_key_slot;

/*
 * Run destructors for one thread. On each pass, every key that is in use,
 * has a destructor and holds a non-null value has its value cleared and then
 * passed to the destructor. Passes repeat while a pass called any destructor,
 * up to passes times, because a destructor may set a key again. Afterwards
 * every value in the row is null, whatever remains. keys and row must hold
 * key_count entries; destructors may modify either array.
 */
void runtime_tls_finish_row(const runtime_key_slot keys[], void *row[],
                            size_t key_count, size_t passes);

#endif
