/* runtime_ctl_device.h: the /dev/chrysoctl owner machine and the
 * /dev/chrysostat read decision. The gate table defaults a missing cell to
 * ok, so every (phase, event) cell is pinned here, from the owner's fd and a
 * foreign one. */
#include "check.h"

#include "runtime_ctl_device.h"

#include <string.h>

static constexpr int owner_fd = 7;
static constexpr int other_fd = 9;
static constexpr size_t record = sizeof(chryso_ctl_command);
static constexpr uint64_t no_values[runtime_ctl_words] = {};

typedef enum : uint8_t {
  ev_open,
  ev_write,
  ev_reply,
  ev_read,
  ev_close,
  ev_count,
} event;

static constexpr size_t phase_count = runtime_ctl_reply_ready + 1;

static chryso_ctl_command valid_command(void) {
  return (chryso_ctl_command){
      .magic = chryso_magic_command,
      .version = (uint16_t)chryso_abi_version,
      .opcode = chryso_pp_opcode_restart_child,
      .args = {1, 2, 3, 4},
  };
}

/* One byte off alignment, as an Erlang binary may be. */
typedef struct {
  uint8_t bytes[record + 1];
} unaligned;

static const uint8_t *place(unaligned *u, const chryso_ctl_command *c) {
  memcpy(&u->bytes[1], c, sizeof(*c));
  return &u->bytes[1];
}

static runtime_ctl_status fire(runtime_ctl_owner *o, int fd, event ev) {
  unaligned u = {};
  const chryso_ctl_command c = valid_command();
  runtime_ctl_call call = {};
  switch (ev) {
  case ev_open:
    return runtime_ctl_open(o, fd);
  case ev_write:
    return runtime_ctl_write(o, fd, place(&u, &c), record, &call);
  case ev_reply:
    return runtime_ctl_reply(o, fd, chryso_ctl_result_ok, runtime_ctl_words,
                             no_values);
  case ev_read:
    return runtime_ctl_read(o, fd, &u.bytes[1], record);
  case ev_close:
    return runtime_ctl_close(o, fd);
  case ev_count:
    break;
  }
  return runtime_ctl_out_of_order;
}

static runtime_ctl_owner in_phase(runtime_ctl_phase phase) {
  runtime_ctl_owner o = {};
  static const event path[] = {ev_open, ev_write, ev_reply};
  for (size_t i = 0; i < (size_t)phase; i++) {
    CHECK_EQ_U64(fire(&o, owner_fd, path[i]), runtime_ctl_ok);
  }
  CHECK_EQ_U64(o.phase, phase);
  return o;
}

typedef struct {
  runtime_ctl_status status;
  runtime_ctl_phase next;
} cell;

/* The header's transition table, from the owner's fd. */
static const cell expected[phase_count][ev_count] = {
    [runtime_ctl_closed] =
        {
            [ev_open] = {runtime_ctl_ok, runtime_ctl_idle},
            [ev_write] = {runtime_ctl_not_owner, runtime_ctl_closed},
            [ev_reply] = {runtime_ctl_not_owner, runtime_ctl_closed},
            [ev_read] = {runtime_ctl_not_owner, runtime_ctl_closed},
            [ev_close] = {runtime_ctl_not_owner, runtime_ctl_closed},
        },
    [runtime_ctl_idle] =
        {
            [ev_open] = {runtime_ctl_busy, runtime_ctl_idle},
            [ev_write] = {runtime_ctl_ok, runtime_ctl_calling},
            [ev_reply] = {runtime_ctl_out_of_order, runtime_ctl_idle},
            [ev_read] = {runtime_ctl_no_reply, runtime_ctl_idle},
            [ev_close] = {runtime_ctl_ok, runtime_ctl_closed},
        },
    [runtime_ctl_calling] =
        {
            [ev_open] = {runtime_ctl_busy, runtime_ctl_calling},
            [ev_write] = {runtime_ctl_busy, runtime_ctl_calling},
            [ev_reply] = {runtime_ctl_ok, runtime_ctl_reply_ready},
            [ev_read] = {runtime_ctl_busy, runtime_ctl_calling},
            [ev_close] = {runtime_ctl_ok, runtime_ctl_closed},
        },
    [runtime_ctl_reply_ready] =
        {
            [ev_open] = {runtime_ctl_busy, runtime_ctl_reply_ready},
            [ev_write] = {runtime_ctl_busy, runtime_ctl_reply_ready},
            [ev_reply] = {runtime_ctl_out_of_order, runtime_ctl_reply_ready},
            [ev_read] = {runtime_ctl_ok, runtime_ctl_idle},
            [ev_close] = {runtime_ctl_ok, runtime_ctl_closed},
        },
};

static void test_owner_table(void) {
  for (size_t p = 0; p < phase_count; p++) {
    for (size_t e = 0; e < ev_count; e++) {
      runtime_ctl_owner o = in_phase((runtime_ctl_phase)p);
      CHECK_EQ_U64(fire(&o, owner_fd, (event)e), expected[p][e].status);
      CHECK_EQ_U64(o.phase, expected[p][e].next);
    }
  }
}

/* Outside closed, only open gets past ownership, and it is busy. */
static void test_foreign_fd(void) {
  for (size_t p = runtime_ctl_idle; p < phase_count; p++) {
    for (size_t e = 0; e < ev_count; e++) {
      runtime_ctl_owner o = in_phase((runtime_ctl_phase)p);
      const runtime_ctl_owner before = o;
      CHECK_EQ_U64(fire(&o, other_fd, (event)e),
                   e == ev_open ? runtime_ctl_busy : runtime_ctl_not_owner);
      CHECK(memcmp(&o, &before, sizeof(o)) == 0);
    }
  }
}

static void test_write_guards(void) {
  runtime_ctl_owner o = in_phase(runtime_ctl_idle);
  runtime_ctl_call call = {};
  unaligned u = {};
  const chryso_ctl_command c = valid_command();
  const uint8_t *bytes = place(&u, &c);
  static const size_t sizes[] = {0, 1, 32, record - 1, record + 1};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    CHECK_EQ_U64(runtime_ctl_write(&o, owner_fd, bytes, sizes[i], &call),
                 runtime_ctl_size);
  }
  CHECK_EQ_U64(runtime_ctl_write(&o, owner_fd, nullptr, record, &call),
               runtime_ctl_size);

  /* One field broken at a time, each caught by the generated checker. */
  chryso_ctl_command bad[6] = {c, c, c, c, c, c};
  bad[0].magic ^= 1;
  bad[1].version = (uint16_t)(chryso_abi_version + 1);
  bad[2].reserved0 = 1;
  bad[3].reserved[15] = 1;
  bad[4].opcode = chryso_pp_opcode_unset;
  bad[5].opcode = 99;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    CHECK_EQ_U64(
        runtime_ctl_write(&o, owner_fd, place(&u, &bad[i]), record, &call),
        runtime_ctl_malformed);
  }
  CHECK_EQ_U64(o.phase, runtime_ctl_idle);

  CHECK_EQ_U64(runtime_ctl_write(&o, owner_fd, place(&u, &c), record, &call),
               runtime_ctl_ok);
  CHECK_EQ_U64(call.opcode, chryso_pp_opcode_restart_child);
  for (size_t i = 0; i < runtime_ctl_words; i++) {
    CHECK_EQ_U64(call.args[i], i + 1);
  }
}

static void test_reply_frames(void) {
  static const struct {
    uint64_t label;
    uint64_t count;
  } invalid[] = {
      {0, runtime_ctl_words},
      {chryso_ctl_result_stale + 1, runtime_ctl_words},
      {UINT64_C(0x10001), runtime_ctl_words},
      {chryso_ctl_result_ok, runtime_ctl_words - 1},
      {chryso_ctl_result_ok, runtime_ctl_words + 1},
      {chryso_ctl_result_ok, 0},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    runtime_ctl_owner o = in_phase(runtime_ctl_calling);
    CHECK_EQ_U64(runtime_ctl_reply(&o, owner_fd, invalid[i].label,
                                   invalid[i].count, no_values),
                 runtime_ctl_unavailable);
    CHECK_EQ_U64(o.phase, runtime_ctl_idle);
    CHECK_EQ_U64(fire(&o, owner_fd, ev_read), runtime_ctl_no_reply);
  }

  static constexpr uint64_t values[runtime_ctl_words] = {11, 22, 33, 44};
  for (uint64_t r = chryso_ctl_result_ok; r <= chryso_ctl_result_stale; r++) {
    runtime_ctl_owner o = in_phase(runtime_ctl_calling);
    CHECK_EQ_U64(runtime_ctl_reply(&o, owner_fd, r, runtime_ctl_words, values),
                 runtime_ctl_ok);
    unaligned u = {};
    CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, &u.bytes[1], record),
                 runtime_ctl_ok);
    CHECK_EQ_U64(chryso_check_ctl_reply(&u.bytes[1], record),
                 chryso_abi_reject_ok);
    chryso_ctl_reply reply = {};
    memcpy(&reply, &u.bytes[1], sizeof(reply));
    CHECK_EQ_U64(reply.result, r);
    for (size_t i = 0; i < runtime_ctl_words; i++) {
      CHECK_EQ_U64(reply.values[i], values[i]);
    }
    CHECK_EQ_U64(o.phase, runtime_ctl_idle);
  }
}

static void test_read_guards(void) {
  runtime_ctl_owner o = in_phase(runtime_ctl_reply_ready);
  unaligned u = {};
  CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, &u.bytes[1], record - 1),
               runtime_ctl_size);
  CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, &u.bytes[1], record + 1),
               runtime_ctl_size);
  CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, nullptr, record),
               runtime_ctl_size);
  CHECK_EQ_U64(o.phase, runtime_ctl_reply_ready);
  CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, &u.bytes[1], record),
               runtime_ctl_ok);
  CHECK_EQ_U64(runtime_ctl_read(&o, owner_fd, &u.bytes[1], record),
               runtime_ctl_no_reply);
}

/* A reply dropped by close never reaches the next opener. */
static void test_close_drops_reply(void) {
  runtime_ctl_owner o = in_phase(runtime_ctl_reply_ready);
  CHECK_EQ_U64(runtime_ctl_close(&o, owner_fd), runtime_ctl_ok);
  CHECK_EQ_U64(runtime_ctl_open(&o, other_fd), runtime_ctl_ok);
  CHECK_EQ_U64(fire(&o, other_fd, ev_read), runtime_ctl_no_reply);
  CHECK_EQ_U64(o.reply.magic, 0);
}

static void test_corrupt_phase(void) {
  for (size_t e = 0; e < ev_count; e++) {
    runtime_ctl_owner o = {.phase = (runtime_ctl_phase)200, .fd = owner_fd};
    CHECK_EQ_U64(fire(&o, owner_fd, (event)e), runtime_ctl_out_of_order);
    CHECK_EQ_U64(o.phase, 200);
  }
}

static void test_stat_decide(void) {
  static constexpr size_t page = sizeof(chryso_root_status_page);
  CHECK_EQ_U64(runtime_stat_read_decide(page, 0, page), runtime_stat_copy);
  CHECK_EQ_U64(runtime_stat_read_decide(page + 1, 0, page), runtime_stat_copy);
  CHECK_EQ_U64(runtime_stat_read_decide(page - 1, 0, page), runtime_stat_short);
  CHECK_EQ_U64(runtime_stat_read_decide(0, 0, page), runtime_stat_short);
  CHECK_EQ_U64(runtime_stat_read_decide(page, page, page), runtime_stat_eof);
  CHECK_EQ_U64(runtime_stat_read_decide(1, 1, page), runtime_stat_eof);
}

int main(void) {
  test_owner_table();
  test_foreign_fd();
  test_write_guards();
  test_reply_frames();
  test_read_guards();
  test_close_drops_reply();
  test_corrupt_phase();
  test_stat_decide();
  return check_finish("ctl_device");
}
