/*
 * The key table and the row are reread on every step because a destructor may
 * call pthread_setspecific or pthread_key_delete while the loop runs.
 */
#include "runtime_tls_row.h"

void runtime_tls_finish_row(const runtime_key_slot keys[], void *row[],
                            size_t key_count, size_t passes) {
  for (size_t pass = 0; pass < passes; pass++) {
    bool called = false;
    for (size_t key = 0; key < key_count; key++) {
      void *value = row[key];
      void (*destructor)(void *) = keys[key].destructor;
      if (keys[key].used && value != nullptr && destructor != nullptr) {
        row[key] = nullptr;
        destructor(value);
        called = true;
      }
    }
    if (!called) {
      break;
    }
  }
  for (size_t key = 0; key < key_count; key++) {
    row[key] = nullptr;
  }
}
