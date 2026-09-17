/* runtime_fd_pair.h: pipe2/socketpair allocation and exhaustion rollback. */
#include "check.h"

#include "runtime_fd_pair.h"

#include <stddef.h>

enum : size_t { table_size = 8 };
enum : int { fake_exhausted = -24 };

/* A descriptor table with a configurable number of free slots. */
typedef struct {
  bool open[table_size];
  size_t free_slots;
  size_t allocs;
  size_t closes;
  size_t which_seen[2];
  bool bad_close;
} fake_table;

static int fake_alloc(void *context, size_t which) {
  fake_table *table = context;
  table->allocs++;
  if (which < 2) {
    table->which_seen[which]++;
  }
  if (table->free_slots == 0) {
    return fake_exhausted;
  }
  for (size_t fd = 0; fd < table_size; fd++) {
    if (!table->open[fd]) {
      table->open[fd] = true;
      table->free_slots--;
      return (int)fd;
    }
  }
  return fake_exhausted;
}

static int fake_close(void *context, int fd) {
  fake_table *table = context;
  table->closes++;
  if (fd < 0 || (size_t)fd >= table_size || !table->open[fd]) {
    table->bad_close = true;
    return -9;
  }
  table->open[fd] = false;
  table->free_slots++;
  return 0;
}

static size_t open_count(const fake_table *table) {
  size_t n = 0;
  for (size_t fd = 0; fd < table_size; fd++) {
    n += table->open[fd] ? 1 : 0;
  }
  return n;
}

static runtime_fd_pair_ops ops_for(fake_table *table) {
  return (runtime_fd_pair_ops){
      .alloc = fake_alloc, .close = fake_close, .context = table};
}

static void test_success(void) {
  fake_table table = {.free_slots = table_size};
  const runtime_fd_pair_ops ops = ops_for(&table);
  int out[2] = {-1, -1};

  CHECK(runtime_fd_alloc_pair(&ops, out) == 0);
  CHECK(out[0] == 0 && out[1] == 1);
  CHECK_EQ_U64(table.which_seen[0], 1);
  CHECK_EQ_U64(table.which_seen[1], 1);
  CHECK_EQ_U64(table.closes, 0);
  CHECK_EQ_U64(open_count(&table), 2);
}

static void test_first_allocation_fails(void) {
  fake_table table = {.free_slots = 0};
  const runtime_fd_pair_ops ops = ops_for(&table);
  int out[2] = {-7, -7};

  CHECK(runtime_fd_alloc_pair(&ops, out) == fake_exhausted);
  CHECK_EQ_U64(table.allocs, 1);
  CHECK_EQ_U64(table.closes, 0);
  CHECK(out[0] == -7 && out[1] == -7);
}

/* One free slot: the first descriptor must go back when the second fails. */
static void test_second_allocation_fails(void) {
  fake_table table = {.free_slots = 1};
  const runtime_fd_pair_ops ops = ops_for(&table);
  int out[2] = {-7, -7};

  CHECK(runtime_fd_alloc_pair(&ops, out) == fake_exhausted);
  CHECK_EQ_U64(table.allocs, 2);
  CHECK_EQ_U64(table.closes, 1);
  CHECK(!table.bad_close);
  CHECK_EQ_U64(open_count(&table), 0);
  CHECK_EQ_U64(table.free_slots, 1);
  CHECK(out[0] == -7 && out[1] == -7);
}

/* Fill the table pair by pair; the failing attempt leaves it exactly full
 * with no leaked slot, and freeing a pair makes room again. */
static void test_exhaustion_cycle(void) {
  fake_table table = {.free_slots = table_size - 1};
  const runtime_fd_pair_ops ops = ops_for(&table);
  int pairs[table_size][2] = {};
  size_t made = 0;
  while (runtime_fd_alloc_pair(&ops, pairs[made]) == 0) {
    made++;
  }
  CHECK_EQ_U64(made, (table_size - 1) / 2);
  CHECK_EQ_U64(open_count(&table), made * 2);
  CHECK(!table.bad_close);

  for (size_t round = 0; round < 3; round++) {
    CHECK(fake_close(&table, pairs[0][0]) == 0);
    CHECK(fake_close(&table, pairs[0][1]) == 0);
    CHECK(runtime_fd_alloc_pair(&ops, pairs[0]) == 0);
    CHECK(runtime_fd_alloc_pair(&ops, pairs[made]) == fake_exhausted);
    CHECK_EQ_U64(open_count(&table), made * 2);
  }
  CHECK(!table.bad_close);
}

int main(void) {
  test_success();
  test_first_allocation_fails();
  test_second_allocation_fails();
  test_exhaustion_cycle();
  return check_finish("fd_pair");
}
