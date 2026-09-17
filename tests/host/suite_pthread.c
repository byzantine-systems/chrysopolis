/* runtime_token_counter.h and runtime_tls_row.h: thread identity and
 * thread-specific data teardown. */
#include "check.h"

#include "runtime_tls_row.h"
#include "runtime_token_counter.h"

#include <stdint.h>

static void test_tokens_never_repeat(void) {
  runtime_token_counter counter = {.next = 32};
  uintptr_t previous = 0;
  for (size_t i = 0; i < 100; i++) {
    uintptr_t token = 0;
    CHECK(runtime_token_counter_next(&counter, &token));
    CHECK(i == 0 || token == previous + 1);
    previous = token;
  }
  CHECK_EQ_U64(previous, 131);
}

/* The last two values: UINTPTR_MAX - 1 is handed out, UINTPTR_MAX never is,
 * and exhaustion is sticky without touching the output. */
static void test_token_exhaustion(void) {
  runtime_token_counter counter = {.next = UINTPTR_MAX - 1};
  uintptr_t token = 0;
  CHECK(runtime_token_counter_next(&counter, &token));
  CHECK(token == UINTPTR_MAX - 1);
  for (size_t i = 0; i < 3; i++) {
    token = 7;
    CHECK(!runtime_token_counter_next(&counter, &token));
    CHECK_EQ_U64(token, 7);
  }
}

enum : size_t { key_count = 4 };

/* Destructor bookkeeping shared by the fakes below. */
static size_t destructor_calls;
static void *last_destroyed;
static void **reset_row;
static size_t resets_left;
static runtime_key_slot *mutable_keys;

static void count_destructor(void *value) {
  destructor_calls++;
  last_destroyed = value;
}

/* Sets key 0 again while resets_left lasts, as a pthread_setspecific call from
 * a destructor would. */
static void resetting_destructor(void *value) {
  destructor_calls++;
  if (resets_left > 0) {
    resets_left--;
    reset_row[0] = value;
  }
}

/* Deletes key 1 from inside a destructor, as pthread_key_delete would. */
static void deleting_destructor(void *value) {
  (void)value;
  destructor_calls++;
  mutable_keys[1] = (runtime_key_slot){};
}

static bool row_is_clear(void *const row[]) {
  for (size_t i = 0; i < key_count; i++) {
    if (row[i] != nullptr) {
      return false;
    }
  }
  return true;
}

static void test_destructor_rules(void) {
  int values[key_count] = {};
  const runtime_key_slot keys[key_count] = {
      {.destructor = count_destructor, .used = true},
      /* No destructor: value dropped silently. */
      {.destructor = nullptr, .used = true},
      /* Deleted key: its destructor must not run. */
      {.destructor = count_destructor, .used = false},
      {.destructor = count_destructor, .used = true},
  };
  void *row[key_count] = {&values[0], &values[1], &values[2], nullptr};

  destructor_calls = 0;
  runtime_tls_finish_row(keys, row, key_count, 4);
  CHECK_EQ_U64(destructor_calls, 1);
  CHECK(last_destroyed == &values[0]);
  CHECK(row_is_clear(row));
}

/* A destructor that keeps setting its key is bounded by the pass count, and
 * whatever it leaves behind is still cleared. */
static void test_pass_limit(void) {
  static int value = 0;
  for (size_t resets = 0; resets < 8; resets++) {
    const runtime_key_slot keys[key_count] = {
        {.destructor = resetting_destructor, .used = true},
    };
    void *row[key_count] = {&value};
    destructor_calls = 0;
    reset_row = row;
    resets_left = resets;

    runtime_tls_finish_row(keys, row, key_count, 4);
    CHECK_EQ_U64(destructor_calls, resets < 4 ? resets + 1 : 4);
    CHECK(row_is_clear(row));
  }

  const runtime_key_slot keys[key_count] = {
      {.destructor = count_destructor, .used = true},
  };
  void *row[key_count] = {&value};
  destructor_calls = 0;
  runtime_tls_finish_row(keys, row, key_count, 0);
  CHECK_EQ_U64(destructor_calls, 0);
  CHECK(row_is_clear(row));
}

static void test_key_deleted_during_teardown(void) {
  int values[2] = {};
  runtime_key_slot keys[key_count] = {
      {.destructor = deleting_destructor, .used = true},
      {.destructor = count_destructor, .used = true},
  };
  void *row[key_count] = {&values[0], &values[1]};
  mutable_keys = keys;
  destructor_calls = 0;

  runtime_tls_finish_row(keys, row, key_count, 4);
  CHECK_EQ_U64(destructor_calls, 1);
  CHECK(row_is_clear(row));
}

int main(void) {
  test_tokens_never_repeat();
  test_token_exhaustion();
  test_destructor_rules();
  test_pass_limit();
  test_key_deleted_during_teardown();
  return check_finish("pthread");
}
