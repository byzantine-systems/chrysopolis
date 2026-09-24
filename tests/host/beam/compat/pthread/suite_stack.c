/* runtime_stack.h: all-or-nothing stack ownership under injected failures. */
#include "check.h"

#include "runtime_stack.h"

#include <stdint.h>

enum : size_t { fake_capacity = 64 };

/*
 * Deterministic allocator. runtime_stack_prepare_n never dereferences a stack,
 * so the fake hands out distinct addresses of one static array and records
 * every call instead of reserving real memory.
 */
typedef struct {
  size_t calls;
  /* Allocation number that returns nullptr; SIZE_MAX never fails. */
  size_t fail_at;
  /* Allocation number that returns an address whose end wraps; SIZE_MAX never.
   */
  size_t wrap_at;
  size_t live;
  uintptr_t handed[fake_capacity];
  bool freed[fake_capacity];
  uintptr_t release_order[fake_capacity];
  size_t releases;
  /* A release of an unknown or already released pointer. */
  bool bad_release;
} fake_allocator;

static char fake_arena[fake_capacity];

/* With exactly runtime_stack_min_bytes this address ends at UINTPTR_MAX, and
 * one byte more wraps. */
static constexpr uintptr_t wrapping_address =
    UINTPTR_MAX - runtime_stack_min_bytes;

static void *fake_alloc(size_t bytes, void *context) {
  (void)bytes;
  fake_allocator *fake = context;
  const size_t n = fake->calls++;
  if (n == fake->fail_at || n >= fake_capacity) {
    return nullptr;
  }
  /* Converting an integer to a pointer is implementation-defined; the unit
   * only compares the value and hands it back to release. */
  void *pointer =
      n == fake->wrap_at ? (void *)wrapping_address : &fake_arena[n];
  fake->handed[n] = (uintptr_t)pointer;
  fake->live++;
  return pointer;
}

static void fake_free(void *pointer, void *context) {
  fake_allocator *fake = context;
  for (size_t i = 0; i < fake->calls && i < fake_capacity; i++) {
    if (fake->handed[i] == (uintptr_t)pointer && !fake->freed[i]) {
      fake->freed[i] = true;
      fake->live--;
      fake->release_order[fake->releases++] = (uintptr_t)pointer;
      return;
    }
  }
  fake->bad_release = true;
}

static fake_allocator fake_new(void) {
  return (fake_allocator){.fail_at = SIZE_MAX, .wrap_at = SIZE_MAX};
}

static void fill(uintptr_t stacks[], size_t count, uintptr_t value) {
  for (size_t i = 0; i < count; i++) {
    stacks[i] = value;
  }
}

static bool all_equal(const uintptr_t stacks[], size_t count, uintptr_t value) {
  for (size_t i = 0; i < count; i++) {
    if (stacks[i] != value) {
      return false;
    }
  }
  return true;
}

static void test_invalid_arguments(void) {
  enum : size_t { count = 4 };
  uintptr_t stacks[count] = {};
  fill(stacks, count, UINTPTR_MAX);
  fake_allocator fake = fake_new();
  const size_t min = runtime_stack_min_bytes;

  CHECK(!runtime_stack_prepare_n(nullptr, count, min, fake_alloc, fake_free,
                                 &fake));
  CHECK(!runtime_stack_prepare_n(stacks, 0, min, fake_alloc, fake_free, &fake));
  CHECK(!runtime_stack_prepare_n(stacks, count, min - 1, fake_alloc, fake_free,
                                 &fake));
  CHECK(
      !runtime_stack_prepare_n(stacks, count, min, nullptr, fake_free, &fake));
  CHECK(
      !runtime_stack_prepare_n(stacks, count, min, fake_alloc, nullptr, &fake));

  CHECK_EQ_U64(fake.calls, 0);
  CHECK(all_equal(stacks, count, UINTPTR_MAX));
}

static void test_success(void) {
  enum : size_t { count = 8 };
  uintptr_t stacks[count] = {};
  fake_allocator fake = fake_new();

  CHECK(runtime_stack_prepare_n(stacks, count, runtime_stack_min_bytes,
                                fake_alloc, fake_free, &fake));
  CHECK_EQ_U64(fake.live, count);
  CHECK_EQ_U64(fake.releases, 0);
  for (size_t i = 0; i < count; i++) {
    CHECK(stacks[i] == fake.handed[i]);
    for (size_t j = 0; j < i; j++) {
      CHECK(stacks[i] != stacks[j]);
    }
  }
}

/* Failing the Nth allocation releases exactly the N earlier ones, newest
 * first, and clears every slot. */
static void test_allocation_failure_rollback(void) {
  enum : size_t { count = 8 };
  for (size_t fail_at = 0; fail_at < count; fail_at++) {
    uintptr_t stacks[count] = {};
    fill(stacks, count, UINTPTR_MAX);
    fake_allocator fake = fake_new();
    fake.fail_at = fail_at;

    CHECK(!runtime_stack_prepare_n(stacks, count, runtime_stack_min_bytes,
                                   fake_alloc, fake_free, &fake));
    CHECK_EQ_U64(fake.calls, fail_at + 1);
    CHECK_EQ_U64(fake.live, 0);
    CHECK_EQ_U64(fake.releases, fail_at);
    CHECK(!fake.bad_release);
    CHECK(all_equal(stacks, count, 0));
    for (size_t r = 0; r < fake.releases; r++) {
      CHECK(fake.release_order[r] == fake.handed[fail_at - 1 - r]);
    }
  }
}

/* An allocation whose end would wrap is released itself, then rolled back. */
static void test_wrapping_allocation(void) {
  enum : size_t { count = 4 };
  for (size_t wrap_at = 0; wrap_at < count; wrap_at++) {
    uintptr_t stacks[count] = {};
    fake_allocator fake = fake_new();
    fake.wrap_at = wrap_at;

    CHECK(!runtime_stack_prepare_n(stacks, count, runtime_stack_min_bytes + 1,
                                   fake_alloc, fake_free, &fake));
    CHECK_EQ_U64(fake.live, 0);
    CHECK_EQ_U64(fake.releases, wrap_at + 1);
    CHECK(fake.release_order[0] == wrapping_address);
    CHECK(!fake.bad_release);
    CHECK(all_equal(stacks, count, 0));
  }

  /* The bound is exact: an end address of UINTPTR_MAX itself is accepted. */
  uintptr_t one[1] = {};
  fake_allocator fake = fake_new();
  fake.wrap_at = 0;
  CHECK(runtime_stack_prepare_n(one, 1, runtime_stack_min_bytes, fake_alloc,
                                fake_free, &fake));
  CHECK(one[0] == wrapping_address);
}

/* Preparing, releasing and preparing again leaves no residue. */
static void test_repeated_cycles(void) {
  enum : size_t { count = 3, cycles = 5 };
  fake_allocator fake = fake_new();
  for (size_t cycle = 0; cycle < cycles; cycle++) {
    uintptr_t stacks[count] = {};
    CHECK(runtime_stack_prepare_n(stacks, count, runtime_stack_min_bytes,
                                  fake_alloc, fake_free, &fake));
    CHECK_EQ_U64(fake.live, count);
    for (size_t i = 0; i < count; i++) {
      fake_free((void *)stacks[i], &fake);
    }
    CHECK_EQ_U64(fake.live, 0);
  }
  CHECK(!fake.bad_release);
}

int main(void) {
  test_invalid_arguments();
  test_success();
  test_allocation_failure_rollback();
  test_wrapping_allocation();
  test_repeated_cycles();
  return check_finish("stack");
}
