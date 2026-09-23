/* runtime_pd_restart_parse.h: /dev/pd-restart payload decoding and gating. */
#include "check.h"

#include "runtime_pd_restart_parse.h"

#include <stdint.h>
#include <string.h>

static const char *const names[] = {"serial", "timer", "blk", "eth"};
static constexpr size_t class_count = sizeof(names) / sizeof(names[0]);
static constexpr size_t unset = SIZE_MAX;

static constexpr size_t healthy = runtime_pd_restart_mode_healthy;
static constexpr size_t fault = runtime_pd_restart_mode_fault;

/* Parse a string literal payload without its terminator. */
static bool parse(const char *text, size_t *mode, size_t *class_index) {
  *mode = unset;
  *class_index = unset;
  return runtime_pd_restart_parse(text, strlen(text), names, class_count, mode,
                                  class_index) == runtime_pd_restart_parse_ok;
}

static void test_healthy_classes(void) {
  size_t mode = 0;
  size_t class_index = 0;
  for (size_t i = 0; i < class_count; i++) {
    CHECK(parse(names[i], &mode, &class_index));
    CHECK_EQ_U64(mode, healthy);
    CHECK_EQ_U64(class_index, i);
  }
}

static void test_fault_prefix_and_whitespace(void) {
  size_t mode = 0;
  size_t class_index = 0;
  CHECK(parse("fault:blk", &mode, &class_index));
  CHECK_EQ_U64(mode, fault);
  CHECK_EQ_U64(class_index, 2);

  CHECK(parse("eth\n", &mode, &class_index));
  CHECK_EQ_U64(class_index, 3);
  CHECK(parse("fault:timer \r\n\t", &mode, &class_index));
  CHECK_EQ_U64(mode, fault);
  CHECK_EQ_U64(class_index, 1);
}

static void test_rejections_leave_outputs(void) {
  static const char *const rejected[] = {
      "",       "\n \t\r", "fault:",      "fault:\n", "fault",
      "ser",    "serials", " serial",     "SERIAL",   "fault:fault:blk",
      "blk\nx", "fault:x", "restart:blk",
  };
  for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
    size_t mode = 0;
    size_t class_index = 0;
    CHECK(!parse(rejected[i], &mode, &class_index));
    CHECK_EQ_U64(mode, unset);
    CHECK_EQ_U64(class_index, unset);
  }

  /* An empty write may pass a null buffer. */
  size_t mode = unset;
  size_t class_index = unset;
  CHECK(runtime_pd_restart_parse(nullptr, 0, names, class_count, &mode,
                                 &class_index) ==
        runtime_pd_restart_parse_unknown);
  CHECK_EQ_U64(mode, unset);
}

/* Lengths bound the comparison: an embedded NUL does not end a match, and
 * bytes after count are never read as part of the name. */
static void test_length_bounded_compare(void) {
  size_t mode = unset;
  size_t class_index = unset;
  static constexpr char with_nul[] = {'b', 'l', 'k', '\0'};
  CHECK(runtime_pd_restart_parse(with_nul, sizeof(with_nul), names, class_count,
                                 &mode, &class_index) ==
        runtime_pd_restart_parse_unknown);

  static constexpr char prefix_of_longer[] = {'b', 'l', 'k', 'x', 'y'};
  CHECK(runtime_pd_restart_parse(prefix_of_longer, 3, names, class_count, &mode,
                                 &class_index) == runtime_pd_restart_parse_ok);
  CHECK_EQ_U64(class_index, 2);
}

static void test_any_channel(void) {
  static constexpr uint8_t none = 0xff;
  volatile uint8_t channels[2][4] = {
      {none, none, none, none},
      {none, none, none, none},
  };
  CHECK(!runtime_pd_restart_any_channel(2, 4, channels, none));

  /* Any single wired entry, in either row, enables the trigger. */
  for (size_t mode = 0; mode < 2; mode++) {
    for (size_t class_index = 0; class_index < 4; class_index++) {
      channels[mode][class_index] = 7;
      CHECK(runtime_pd_restart_any_channel(2, 4, channels, none));
      channels[mode][class_index] = none;
    }
  }
  /* Channel 0 is a real channel id, not "absent". */
  channels[1][3] = 0;
  CHECK(runtime_pd_restart_any_channel(2, 4, channels, none));
  CHECK(!runtime_pd_restart_any_channel(1, 4, channels, none));
}

int main(void) {
  test_healthy_classes();
  test_fault_prefix_and_whitespace();
  test_rejections_leave_outputs();
  test_length_bounded_compare();
  test_any_channel();
  return check_finish("pd_restart");
}
