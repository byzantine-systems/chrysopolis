/* runtime_epoll_table.h: registration, readiness filtering and ONESHOT. */
#include "check.h"

#include "runtime_epoll_table.h"

#include <stddef.h>
#include <stdint.h>

enum : size_t { capacity = 4, max_fd = 16 };

static constexpr uint32_t in = runtime_epoll_in;
static constexpr uint32_t out = runtime_epoll_out;
static constexpr uint32_t err = runtime_epoll_err;
static constexpr uint32_t hup = runtime_epoll_hup;
static constexpr uint32_t oneshot = runtime_epoll_oneshot;

typedef struct {
  /* Conditions each fd currently has. */
  uint32_t ready[max_fd];
  size_t readiness_calls;
  size_t reports;
  uint32_t events[capacity];
  uint64_t data[capacity];
  bool bad_index;
} fake_world;

static uint32_t fake_readiness(void *context,
                               const runtime_epoll_entry *entry) {
  fake_world *world = context;
  world->readiness_calls++;
  return entry->fd >= 0 && (size_t)entry->fd < max_fd ? world->ready[entry->fd]
                                                      : 0;
}

static void fake_report(void *context, size_t index, uint32_t events,
                        uint64_t data) {
  fake_world *world = context;
  if (index != world->reports || index >= capacity) {
    world->bad_index = true;
    return;
  }
  world->events[index] = events;
  world->data[index] = data;
  world->reports++;
}

static size_t scan(runtime_epoll_entry table[], fake_world *world, size_t max) {
  world->reports = 0;
  const runtime_epoll_scan_ops ops = {
      .readiness = fake_readiness, .report = fake_report, .context = world};
  return runtime_epoll_table_scan(table, capacity, &ops, max);
}

static void test_set_and_update(void) {
  runtime_epoll_entry table[capacity] = {};
  CHECK(runtime_epoll_table_set(table, capacity, 3, in, 30, false, 99) ==
        runtime_epoll_ok);
  CHECK(table[0].active && table[0].armed && table[0].fd == 3);
  /* The socket index is only kept for sockets. */
  CHECK(table[0].sock_handle == -1);

  /* ADD of a registered fd updates the same entry instead of adding one. */
  CHECK(runtime_epoll_table_set(table, capacity, 3, out, 31, true, 5) ==
        runtime_epoll_ok);
  CHECK(!table[1].active);
  CHECK(table[0].events == out && table[0].data == 31);
  CHECK(table[0].is_sock && table[0].sock_handle == 5);
}

static void test_capacity_and_reuse(void) {
  runtime_epoll_entry table[capacity] = {};
  for (int fd = 0; fd < (int)capacity; fd++) {
    CHECK(runtime_epoll_table_set(table, capacity, fd, in, (uint64_t)fd, false,
                                  -1) == runtime_epoll_ok);
  }

  runtime_epoll_entry before[capacity] = {};
  for (size_t i = 0; i < capacity; i++) {
    before[i] = table[i];
  }
  CHECK(runtime_epoll_table_set(table, capacity, 9, in, 9, false, -1) ==
        runtime_epoll_no_space);
  for (size_t i = 0; i < capacity; i++) {
    CHECK(table[i].fd == before[i].fd && table[i].data == before[i].data &&
          table[i].active == before[i].active);
  }

  /* A full table still accepts an update of a registered fd. */
  CHECK(runtime_epoll_table_set(table, capacity, 2, out, 22, false, -1) ==
        runtime_epoll_ok);

  /* Removing an unknown fd is a silent success; removing a known one frees
   * its slot for the next registration. */
  runtime_epoll_table_remove(table, capacity, 9);
  runtime_epoll_table_remove(table, capacity, 1);
  CHECK(!table[1].active);
  CHECK(runtime_epoll_table_set(table, capacity, 9, in, 9, false, -1) ==
        runtime_epoll_ok);
  CHECK(table[1].active && table[1].fd == 9);
}

static void test_scan_filters_conditions(void) {
  runtime_epoll_entry table[capacity] = {};
  fake_world world = {};
  CHECK(runtime_epoll_table_set(table, capacity, 1, in, 100, false, -1) ==
        runtime_epoll_ok);
  CHECK(runtime_epoll_table_set(table, capacity, 2, out, 200, false, -1) ==
        runtime_epoll_ok);
  CHECK(runtime_epoll_table_set(table, capacity, 3, in, 300, false, -1) ==
        runtime_epoll_ok);

  /* fd 1 only has an unrequested OUT: nothing. fd 2 has OUT plus HUP and ERR,
   * which are always reported. fd 3 is idle. */
  world.ready[1] = out;
  world.ready[2] = out | hup | err | in;
  CHECK_EQ_U64(scan(table, &world, capacity), 1);
  CHECK(world.events[0] == (out | hup | err));
  CHECK_EQ_U64(world.data[0], 200);
  CHECK(!world.bad_index);

  /* An unrequested ERR alone is still reported. */
  world.ready[3] = err;
  CHECK_EQ_U64(scan(table, &world, capacity), 2);
  CHECK(world.events[1] == err);
  CHECK_EQ_U64(world.data[1], 300);
}

/* max_reports stops the scan early, and entries past the limit keep their
 * ONESHOT arming for the next call. */
static void test_report_limit(void) {
  runtime_epoll_entry table[capacity] = {};
  fake_world world = {};
  for (int fd = 0; fd < (int)capacity; fd++) {
    CHECK(runtime_epoll_table_set(table, capacity, fd, in | oneshot,
                                  (uint64_t)fd, false, -1) == runtime_epoll_ok);
    world.ready[fd] = in;
  }
  CHECK_EQ_U64(scan(table, &world, 0), 0);
  CHECK_EQ_U64(world.readiness_calls, 0);

  CHECK_EQ_U64(scan(table, &world, 2), 2);
  CHECK(!table[0].armed && !table[1].armed);
  CHECK(table[2].armed && table[3].armed);
  CHECK_EQ_U64(scan(table, &world, capacity), 2);
  CHECK_EQ_U64(world.data[0], 2);
  CHECK_EQ_U64(world.data[1], 3);
  CHECK_EQ_U64(scan(table, &world, capacity), 0);
}

/* ONESHOT reports once per arming, repeatedly, and a removed entry stays
 * silent. Level-triggered entries report every scan. */
static void test_oneshot_rearm_cycles(void) {
  runtime_epoll_entry table[capacity] = {};
  fake_world world = {};
  world.ready[0] = in;
  world.ready[5] = in;
  CHECK(runtime_epoll_table_set(table, capacity, 0, in | oneshot, 1, false,
                                -1) == runtime_epoll_ok);
  CHECK(runtime_epoll_table_set(table, capacity, 5, in, 2, false, -1) ==
        runtime_epoll_ok);

  for (size_t cycle = 0; cycle < 5; cycle++) {
    CHECK_EQ_U64(scan(table, &world, capacity), 2);
    CHECK_EQ_U64(scan(table, &world, capacity), 1);
    CHECK_EQ_U64(world.data[0], 2);
    /* EPOLL_CTL_MOD re-arms. */
    CHECK(runtime_epoll_table_set(table, capacity, 0, in | oneshot, 1, false,
                                  -1) == runtime_epoll_ok);
  }

  runtime_epoll_table_remove(table, capacity, 0);
  runtime_epoll_table_remove(table, capacity, 5);
  CHECK_EQ_U64(scan(table, &world, capacity), 0);
}

static void test_poll_and_select_helpers(void) {
  CHECK(runtime_poll_wanted_mask(0) == (hup | err));
  CHECK(runtime_poll_wanted_mask(in) == (in | hup | err));
  CHECK(runtime_poll_wanted_mask(in | out | 0x2 | 0x40) ==
        (in | out | hup | err));
  /* A negative short sign-extends to every bit; only IN and OUT survive. */
  CHECK(runtime_poll_wanted_mask(UINT32_MAX) == (in | out | hup | err));

  CHECK(runtime_select_nfds_valid(0, 1024));
  CHECK(runtime_select_nfds_valid(1024, 1024));
  CHECK(!runtime_select_nfds_valid(1025, 1024));
  CHECK(!runtime_select_nfds_valid(-1, 1024));
  CHECK(!runtime_select_nfds_valid(INT32_MIN, 1024));
}

/* Every combination of the four readiness bits, mapped onto select's sets. */
static void test_select_from_revents(void) {
  /* Nothing ready is ready in no set. */
  runtime_select_ready state = runtime_select_from_revents(0);
  CHECK(!state.read && !state.write && !state.except);

  state = runtime_select_from_revents(in);
  CHECK(state.read && !state.write && !state.except);

  state = runtime_select_from_revents(out);
  CHECK(!state.read && state.write && !state.except);

  state = runtime_select_from_revents(in | out);
  CHECK(state.read && state.write && !state.except);

  /* A pending error or a hangup makes the descriptor ready in both sets, so
   * a caller watching either one wakes and collects it. Reporting them only
   * in exceptfds would leave a caller watching read and write blocked. */
  state = runtime_select_from_revents(err);
  CHECK(state.read && state.write && !state.except);

  state = runtime_select_from_revents(hup);
  CHECK(state.read && state.write && !state.except);

  state = runtime_select_from_revents(err | hup);
  CHECK(state.read && state.write && !state.except);

  for (uint32_t bits = 0; bits < 16; bits++) {
    const uint32_t revents = ((bits & 1) ? in : 0) | ((bits & 2) ? out : 0) |
                             ((bits & 4) ? err : 0) | ((bits & 8) ? hup : 0);
    const runtime_select_ready got = runtime_select_from_revents(revents);
    const bool failed = (revents & (err | hup)) != 0;
    CHECK(got.read == (((revents & in) != 0) || failed));
    CHECK(got.write == (((revents & out) != 0) || failed));
    /* This stack's TCP delivers no out-of-band data, so nothing raises the
     * exception set. */
    CHECK(!got.except);
  }

  /* Bits outside the four conditions never make a descriptor ready. */
  state = runtime_select_from_revents(runtime_epoll_oneshot);
  CHECK(!state.read && !state.write && !state.except);
}

int main(void) {
  test_set_and_update();
  test_capacity_and_reuse();
  test_scan_filters_conditions();
  test_report_limit();
  test_oneshot_rearm_cycles();
  test_poll_and_select_helpers();
  test_select_from_revents();
  return check_finish("epoll");
}
